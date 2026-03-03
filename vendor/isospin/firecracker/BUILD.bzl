# Firecracker build with modern 2026 Buck2 patterns

load("@prelude//rust:defs.bzl", "rust_library", "rust_binary", "rust_proc_macro")

# Zero dependency proc-macro - perfect test case
rust_proc_macro(
    name = "log_instrument_macros_lib",
    srcs = glob(["src/**/*.rs"]),
    edition = "2024",
    target_compatible_with = [
        "config//platforms:x86_64-linux", 
        "config//platforms:aarch64-linux",
    ],
)