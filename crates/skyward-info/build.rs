fn main() {
    println!("cargo:rerun-if-env-changed=SKYWARD_BUILDER");
    if let Ok(builder) = std::env::var("SKYWARD_BUILDER") {
        let builder = builder.trim();
        assert!(!builder.contains(['\r', '\n']), "SKYWARD_BUILDER must fit on one line");
        println!("cargo:rustc-env=SKYWARD_BUILDER={builder}");
    }
}
