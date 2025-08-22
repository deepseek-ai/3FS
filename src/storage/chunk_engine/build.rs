fn main() {
    let _ = cxx_build::bridge("src/cxx.rs");
    println!("cargo:rerun-if-changed=src/cxx.rs");
}
