#include <aidl/android/se/omapi/BnSecureElementService.h>
#include <aidl/android/se/omapi/BnSecureElementReader.h>
#include <aidl/android/se/omapi/BnSecureElementSession.h>
#include <aidl/android/se/omapi/BnSecureElementChannel.h>
#include <aidl/android/hardware/secure_element/ISecureElement.h>
#include <aidl/android/hardware/secure_element/BnSecureElementCallback.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <android/binder_ibinder.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>
#include <unistd.h>
#include <dlfcn.h>
#include <fstream>
#include <sstream>
#include <dirent.h>
#include <thread>
#include <chrono>

namespace om = aidl::android::se::omapi;
namespace hw = aidl::android::hardware::secure_element;
using Status = ndk::ScopedAStatus;
static pid_t callerPid;
static uid_t callerUid;
static std::string callerStart;
static constexpr const char* weaverPath="/odm/bin/hw/android.hardware.weaver-service.thales";
static bool isWeaver(pid_t pid) {
    char executable[1024];
    auto length=readlink(("/proc/"+std::to_string(pid)+"/exe").c_str(),executable,sizeof(executable));
    return length>0 && std::string(executable,static_cast<size_t>(length))==weaverPath;
}
static std::string started(pid_t pid) {
    std::ifstream file("/proc/"+std::to_string(pid)+"/stat"); std::string line;
    if(!std::getline(file,line)) return {};
    auto end=line.rfind(')'); if(end==std::string::npos) return {};
    std::istringstream fields(line.substr(end+2)); std::string value;
    for(int field=3;field<=22;++field) if(!(fields>>value)) return {};
    return value;
}
// Platform-only entry points are exported by recovery's libbinder_ndk but
// omitted from the public NDK link stub. Resolve only these exact symbols.
static AIBinder* (*checkService)(const char*);
static binder_status_t (*addService)(AIBinder*,const char*);
static bool (*setThreads)(uint32_t);
static void (*startThreads)();
static void (*joinThreads)();
static void (*markVintf)(AIBinder*);
// The public NDK stub omits this platform entry point. Generated stable
// AIDL stubs must still call the real device implementation.
extern "C" void AIBinder_markVintfStability(AIBinder* binder) {
    if (!markVintf) std::abort();
    markVintf(binder);
}
template<class T> static bool resolve(void* library,const char* name,T& value) {
    value=reinterpret_cast<T>(dlsym(library,name));
    if(!value) std::fprintf(stderr,"Required platform Binder symbol missing: %s\n",name);
    return value!=nullptr;
}
static const std::vector<uint8_t> weaverAid{0xa0,0,0,8,0x44,0x53,0xf1,0x27,0x56,0x18,1,0};
static Status denied(const char* message) { return Status::fromExceptionCodeWithMessage(EX_SECURITY, message); }
static Status checkCaller() {
    if (AIBinder_getCallingPid() != callerPid || AIBinder_getCallingUid() != callerUid || !isWeaver(callerPid) || started(callerPid)!=callerStart)
        return denied("Only the explicitly selected Weaver process and its observed UID are allowed");
    return Status::ok();
}
static Status closed() { return Status::fromExceptionCodeWithMessage(EX_ILLEGAL_STATE, "Session or channel is closed"); }
static Status unsupported() { return Status::fromExceptionCodeWithMessage(EX_UNSUPPORTED_OPERATION, "Operation is outside this diagnostic bridge"); }
struct Context {
    std::shared_ptr<hw::ISecureElement> hal;
    std::mutex io;
    std::atomic<bool> connected{false};
};
class Callback final : public hw::BnSecureElementCallback {
    std::shared_ptr<Context> context;
public:
    explicit Callback(std::shared_ptr<Context> c): context(std::move(c)) {}
    Status onStateChange(bool connected, const std::string&) override {
        context->connected = connected;
        std::fprintf(stderr, "eSE state: %s\n", connected ? "connected" : "disconnected");
        return Status::ok();
    }
};
struct ChannelState {
    std::shared_ptr<Context> context;
    int8_t number;
    std::vector<uint8_t> selectResponse;
    bool isClosed = false;
    std::mutex mutex;
    AIBinder_DeathRecipient* death = nullptr;
    ChannelState(std::shared_ptr<Context> c, hw::LogicalChannelResponse r)
        : context(std::move(c)), number(r.channelNumber), selectResponse(std::move(r.selectResponse)) {}
    Status closeInternal() {
        std::lock_guard<std::mutex> guard(mutex);
        if (isClosed) return Status::ok();
        std::lock_guard<std::mutex> ioGuard(context->io);
        auto result = context->hal->closeChannel(number);
        // No later APDU is allowed once closure has been requested, even on HAL error.
        isClosed = true;
        return result;
    }
    ~ChannelState() {
        if (death) {
            AIBinder_DeathRecipient_delete(death);
        }
        closeInternal();
    }
};
class Channel final : public om::BnSecureElementChannel {
    std::shared_ptr<ChannelState> state;
public:
    explicit Channel(std::shared_ptr<ChannelState> s): state(std::move(s)) {}
    Status close() override { auto s=checkCaller(); return s.isOk() ? state->closeInternal() : std::move(s); }
    Status isClosed(bool* out) override {
        auto s=checkCaller(); if (!s.isOk()) return s;
        std::lock_guard<std::mutex> guard(state->mutex); *out=state->isClosed; return Status::ok();
    }
    Status isBasicChannel(bool* out) override { auto s=checkCaller(); if (!s.isOk()) return s; *out=false; return Status::ok(); }
    Status getSelectResponse(std::vector<uint8_t>* out) override {
        auto s=checkCaller(); if (!s.isOk()) return s;
        std::lock_guard<std::mutex> guard(state->mutex); if (state->isClosed) return closed();
        *out=state->selectResponse; return Status::ok();
    }
    Status transmit(const std::vector<uint8_t>& input, std::vector<uint8_t>* out) override {
        auto s=checkCaller(); if (!s.isOk()) return s;
        if (input.size()<5) return Status::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
        // The observed config and credential read operations use INS 02 and
        // 06. Writes, applet selection, update and channel management never
        // pass through this diagnostic bridge.
        if (input[0]!=0x80 || (input[1]!=0x02 && input[1]!=0x06) || input[2]!=0 || input[3]!=0)
            return denied("Only observed Weaver config and read APDUs are allowed");
        std::lock_guard<std::mutex> guard(state->mutex); if (state->isClosed) return closed();
        auto command=input;
        unsigned channel=static_cast<uint8_t>(state->number);
        if (channel<4) command[0]=(command[0]&0xbc)|channel;
        else if (channel<20) {
            bool sm=(command[0]&0x40)==0 && (command[0]&0x0c)!=0;
            command[0]=(command[0]&0xb0)|0x40|(channel-4); if(sm) command[0]|=0x20;
        } else return Status::fromExceptionCode(EX_ILLEGAL_STATE);
        std::lock_guard<std::mutex> ioGuard(state->context->io);
        auto result=state->context->hal->transmit(command,out);
        std::fprintf(stderr,"Weaver APDU INS=%02x HAL=%s response_bytes=%zu\n",input[1],result.isOk()?"ok":"error",out->size());
        return result;
    }
    Status selectNext(bool*) override { return unsupported(); }
};
class Session final : public om::BnSecureElementSession {
    std::shared_ptr<Context> context;
    std::mutex mutex;
    bool isClosedValue=false;
    std::vector<std::shared_ptr<ChannelState>> channels;
public:
    explicit Session(std::shared_ptr<Context> c): context(std::move(c)) {}
    Status closeInternal(bool closeSession) {
        std::lock_guard<std::mutex> guard(mutex);
        if(closeSession) isClosedValue=true;
        Status first=Status::ok();
        for(auto& ch:channels) { auto s=ch->closeInternal(); if(first.isOk()&&!s.isOk()) first=std::move(s); }
        return first;
    }
    ~Session() { closeInternal(true); }
    Status getAtr(std::vector<uint8_t>* out) override {
        auto s=checkCaller(); if(!s.isOk()) return s;
        std::lock_guard<std::mutex> guard(mutex); if(isClosedValue) return closed();
        std::lock_guard<std::mutex> ioGuard(context->io); return context->hal->getAtr(out);
    }
    Status close() override { auto s=checkCaller(); return s.isOk()?closeInternal(true):std::move(s); }
    Status closeChannels() override { auto s=checkCaller(); return s.isOk()?closeInternal(false):std::move(s); }
    Status isClosed(bool* out) override { auto s=checkCaller(); if(!s.isOk()) return s; std::lock_guard<std::mutex> guard(mutex); *out=isClosedValue; return Status::ok(); }
    Status openBasicChannel(const std::vector<uint8_t>&,int8_t,const std::shared_ptr<om::ISecureElementListener>&,std::shared_ptr<om::ISecureElementChannel>*) override { return unsupported(); }
    Status openLogicalChannel(const std::vector<uint8_t>& aid,int8_t p2,const std::shared_ptr<om::ISecureElementListener>& listener,std::shared_ptr<om::ISecureElementChannel>* out) override {
        auto s=checkCaller(); if(!s.isOk()) return s;
        if(aid!=weaverAid || p2!=0 || !listener) return denied("Only the observed Weaver AID with P2=0 and a live listener is allowed");
        std::lock_guard<std::mutex> guard(mutex); if(isClosedValue) return closed();
        hw::LogicalChannelResponse response;
        { std::lock_guard<std::mutex> ioGuard(context->io); s=context->hal->openLogicalChannel(aid,p2,&response); }
        if(!s.isOk()) { std::fprintf(stderr,"HAL openLogicalChannel failed: %s\n",s.getDescription().c_str()); return s; }
        if(response.channelNumber<=0 || response.channelNumber>19) return Status::fromExceptionCode(EX_ILLEGAL_STATE);
        auto state=std::make_shared<ChannelState>(context,std::move(response));
        std::fprintf(stderr,"HAL logical channel=%d SELECT_bytes=%zu\n",state->number,state->selectResponse.size());
        // The cookie outlives pending death callbacks even when a session is
        // concurrently closed. It owns only a weak state reference.
        auto cookie=new std::weak_ptr<ChannelState>(state);
        state->death=AIBinder_DeathRecipient_new([](void* value){
            if(auto channel=static_cast<std::weak_ptr<ChannelState>*>(value)->lock()) channel->closeInternal();
        });
        AIBinder_DeathRecipient_setOnUnlinked(state->death,[](void* value){delete static_cast<std::weak_ptr<ChannelState>*>(value);});
        auto listenerBinder=listener->asBinder();
        auto link=AIBinder_linkToDeath(listenerBinder.get(),state->death,cookie);
        if(link!=STATUS_OK) return Status::fromStatus(link);
        channels.push_back(state);
        *out=ndk::SharedRefBase::make<Channel>(state);
        return Status::ok();
    }
};
class Reader final : public om::BnSecureElementReader {
    std::shared_ptr<Context> context;
    std::mutex mutex;
    std::vector<std::weak_ptr<Session>> sessions;
public:
    explicit Reader(std::shared_ptr<Context> c): context(std::move(c)) {}
    Status isSecureElementPresent(bool* out) override { auto s=checkCaller(); if(!s.isOk()) return s; std::lock_guard<std::mutex> guard(context->io); return context->hal->isCardPresent(out); }
    Status openSession(std::shared_ptr<om::ISecureElementSession>* out) override {
        auto s=checkCaller(); if(!s.isOk()) return s;
        auto session=ndk::SharedRefBase::make<Session>(context);
        std::lock_guard<std::mutex> guard(mutex); sessions.push_back(session); *out=session; return Status::ok();
    }
    Status closeSessions() override {
        auto s=checkCaller(); if(!s.isOk()) return s;
        std::lock_guard<std::mutex> guard(mutex); Status first=Status::ok();
        for(auto& entry:sessions) if(auto session=entry.lock()) { auto r=session->closeInternal(true); if(first.isOk()&&!r.isOk()) first=std::move(r); }
        return first;
    }
    Status reset(bool*) override { return unsupported(); }
};
class Service final : public om::BnSecureElementService {
    std::shared_ptr<Reader> reader;
public:
    explicit Service(std::shared_ptr<Context> c): reader(ndk::SharedRefBase::make<Reader>(std::move(c))) {}
    Status getReaders(std::vector<std::string>* out) override { auto s=checkCaller(); if(!s.isOk()) return s; *out={"eSE1"}; return Status::ok(); }
    Status getReader(const std::string& name,std::shared_ptr<om::ISecureElementReader>* out) override { auto s=checkCaller(); if(!s.isOk()) return s; if(name!="eSE1") return Status::fromExceptionCode(EX_ILLEGAL_ARGUMENT); *out=reader; return Status::ok(); }
    Status isNfcEventAllowed(const std::string&,const std::vector<uint8_t>&,const std::vector<std::string>& packages,int32_t,std::vector<bool>* out) override { auto s=checkCaller(); if(!s.isOk()) return s; out->assign(packages.size(),false); return Status::ok(); }
};
int main(int argc,char** argv) {
    if(argc!=2) { std::fprintf(stderr,"Usage: omapi-bridge WEAVER_PID\n"); return 2; }
    bool automatic=std::string(argv[1])=="--weaver";
    long pid=0;
    if(automatic) {
        DIR* proc=opendir("/proc"); if(!proc) return 8;
        while(auto* entry=readdir(proc)) {
            char* end=nullptr; long candidate=std::strtol(entry->d_name,&end,10);
            if(end && !*end && candidate>1 && candidate<=INT32_MAX && isWeaver(candidate)) {
                if(pid) { closedir(proc); return 8; }
                pid=candidate;
            }
        }
        closedir(proc); if(!pid) return 8;
    } else {
        char* end=nullptr; pid=std::strtol(argv[1],&end,10);
        if(!end || *end || pid<=1 || pid>INT32_MAX) return 2;
    }
    callerPid=static_cast<pid_t>(pid);
    std::string proc="/proc/"+std::to_string(callerPid);
    char executable[1024]; auto length=readlink((proc+"/exe").c_str(),executable,sizeof(executable));
    if(length<0 || std::string(executable,static_cast<size_t>(length))!=weaverPath) {
        std::fprintf(stderr,"Selected PID is not the observed Thales Weaver executable\n"); return 8;
    }
    std::ifstream processStatus(proc+"/status"); std::string line; bool foundUid=false;
    while(std::getline(processStatus,line)) if(line.rfind("Uid:",0)==0) {
        std::istringstream fields(line.substr(4)); unsigned real,effective,saved,filesystem;
        if(fields>>real>>effective>>saved>>filesystem) { callerUid=effective; foundUid=true; }
        break;
    }
    if(!foundUid) { std::fprintf(stderr,"Unable to determine selected Weaver UID\n"); return 9; }
    if(automatic && callerUid!=2916) { std::fprintf(stderr,"Unexpected dada Weaver UID\n"); return 9; }
    callerStart=started(callerPid); if(callerStart.empty()) return 9;
    void* library=dlopen("libbinder_ndk.so",RTLD_NOW|RTLD_LOCAL);
    if(!library || !resolve(library,"AServiceManager_checkService",checkService)
        || !resolve(library,"AServiceManager_addService",addService)
        || !resolve(library,"ABinderProcess_setThreadPoolMaxThreadCount",setThreads)
        || !resolve(library,"ABinderProcess_startThreadPool",startThreads)
        || !resolve(library,"ABinderProcess_joinThreadPool",joinThreads)
        || !resolve(library,"AIBinder_markVintfStability",markVintf)) return 7;
    constexpr const char* name="android.se.omapi.ISecureElementService/default";
    ndk::SpAIBinder existing(checkService(name));
    if(existing.get()) { std::fprintf(stderr,"OMAPI service already registered\n"); return 3; }
    setThreads(4); startThreads();
    auto context=std::make_shared<Context>();
    context->hal=hw::ISecureElement::fromBinder(ndk::SpAIBinder(checkService("android.hardware.secure_element.ISecureElement/eSE1")));
    if(!context->hal) { std::fprintf(stderr,"Real eSE1 HAL is absent\n"); return 4; }
    auto callback=ndk::SharedRefBase::make<Callback>(context);
    auto status=context->hal->init(callback);
    if(!status.isOk()) { std::fprintf(stderr,"HAL init failed: %s\n",status.getDescription().c_str()); return 5; }
    auto service=ndk::SharedRefBase::make<Service>(context);
    auto binder=service->asBinder();
    auto result=addService(binder.get(),name);
    if(result!=STATUS_OK) { std::fprintf(stderr,"OMAPI registration failed: %d\n",result); return 6; }
    std::fprintf(stderr,"OMAPI bridge registered for Weaver PID %d UID %u\n",callerPid,callerUid);
    auto halBinder=context->hal->asBinder();
    std::thread([halBinder]() {
        for(;;) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if(!isWeaver(callerPid) || started(callerPid)!=callerStart || !AIBinder_isAlive(halBinder.get())) {
                std::fprintf(stderr,"Weaver or eSE restarted; supervisor must establish a new bridge\n");
                _exit(10);
            }
        }
    }).detach();
    joinThreads();
    return 0;
}
