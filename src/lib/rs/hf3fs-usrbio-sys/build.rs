use std::env;
use std::path::PathBuf;

fn main() {
    let topdir = env::var("CARGO_MANIFEST_DIR").unwrap();
    println!("cargo::rustc-link-search=native={}/lib", topdir);
    println!("cargo::rustc-link-lib=hf3fs_api_shared");

    let bindings = bindgen::Builder::default()
        .header(PathBuf::from(topdir).join("../../api/hf3fs_usrbio.h").display().to_string())
        .clang_arg("-std=c99")
        .parse_callbacks(Box::new(bindgen::CargoCallbacks::new()))
        .generate()
        .expect("Unable to generate bindings");

    let out_path = PathBuf::from(env::var("OUT_DIR").unwrap());
    bindings
        .write_to_file(out_path.join("bindings.rs"))
        .expect("Couldn't write bindings!");
}
