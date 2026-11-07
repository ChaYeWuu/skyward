#!/usr/bin/env bash
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
: "${ANDROID_NDK_HOME:?Set ANDROID_NDK_HOME to Android NDK r29}"
: "${ANDROID_HOME:?Set ANDROID_HOME to the Android SDK}"
aidl="$ANDROID_HOME/build-tools/37.0.0/aidl"
compiler="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin/clang++"
mkdir -p "$root/generated" "$root/include" "$root/out"
for kind in omapi hal; do
    folder=android/se/omapi
    if [ "$kind" = hal ]; then folder=android/hardware/secure_element; fi
    hash=$(tr -d '\r\n' < "$root/$kind.hash")
    "$aidl" --lang=ndk --structured --stability=vintf --version=1 \
        "--hash=$hash" --min_sdk_version=35 --omit_invocation \
        "--include=$root/aidl" "--out=$root/generated" \
        "--header_out=$root/include" "$root/aidl/$folder/"*.aidl
done
mapfile -t sources < <(find "$root/generated" -name '*.cpp' -type f | sort)
"$compiler" --target=aarch64-linux-android35 -std=c++17 -O2 \
    -Wall -Wextra -Werror -DBINDER_STABILITY_SUPPORT -fPIE -pie \
    "-I$root/include" "$root/main.cpp" "${sources[@]}" \
    -static-libstdc++ -lbinder_ndk -ldl -o "$root/out/omapi-bridge"
sha256sum "$root/out/omapi-bridge"
