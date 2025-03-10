extern crate bindgen;

use std::env;
use std::path::PathBuf;

fn main() {
    // Tell Cargo to link the 3FS library
    println!("cargo:rustc-link-lib=3fs");

    // Set the path to the 3FS headers
    let headers_path = PathBuf::from("include"); // Adjust this path as needed

    // Generate bindings
    let bindings = bindgen::Builder::default()
        .header(headers_path.join("3fs.h").to_string_lossy())
        .clang_arg(format!("-I{}", headers_path.to_string_lossy()))
        .parse_callbacks(Box::new(bindgen::CargoCallbacks))
        .generate()
        .expect("Unable to generate bindings");

    // Write bindings to a file
    let out_path = PathBuf::from(env::var("OUT_DIR").unwrap());
    bindings
        .write_to_file(out_path.join("bindings.rs"))
        .expect("Couldn't write bindings!");
}
