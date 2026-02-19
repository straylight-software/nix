# toolchains/rust_crate.bzl
#
# Fetch and build crates from crates.io
#
# Simple model:
#   1. http_archive fetches the crate tarball
#   2. rust_crate compiles it with proper flags
#   3. Dependencies are just deps=[]
#
# Example:
#   crates.io_crate(
#       name = "serde",
#       version = "1.0.228",
#       sha256 = "...",
#       features = ["derive"],
#       deps = [":serde_derive"],
#   )

load("@prelude//http_archive.bzl", "http_archive")

# Provider for crate outputs
RustCrateInfo = provider(fields = [
    "rlib",           # Compiled .rlib
    "rmeta",          # Metadata for pipelining
    "crate_name",     # Crate name (underscores)
    "edition",        # Rust edition
    "features",       # Enabled features
    "is_proc_macro",  # Is this a proc-macro crate?
    "transitive_deps", # List of all transitive rlib artifacts (for -L paths)
])

def _crate_url(name: str, version: str) -> str:
    """Get crates.io download URL."""
    return "https://static.crates.io/crates/{}/{}/download".format(name, version)

def _rust_crate_impl(ctx: AnalysisContext) -> list[Provider]:
    """Build a crate from crates.io."""
    
    rustc = read_root_config("rust", "rustc", "rustc")
    
    # Crate name with underscores (Rust convention)
    crate_name = ctx.attrs.crate_name or ctx.attrs.name.replace("-", "_")
    
    # Output files
    if ctx.attrs.proc_macro:
        # Proc macros are .so files
        out = ctx.actions.declare_output("lib{}.so".format(crate_name))
        crate_type = "proc-macro"
    else:
        out = ctx.actions.declare_output("lib{}.rlib".format(crate_name))
        crate_type = "rlib"
    
    cmd = cmd_args([rustc])
    
    # Crate type and name
    cmd.add("--crate-type", crate_type)
    cmd.add("--crate-name", crate_name)
    
    # Edition
    cmd.add("--edition", ctx.attrs.edition)
    
    # Output path
    cmd.add("-o", out.as_output())
    
    # Optimization
    cmd.add("-O")
    
    # Allow warnings (crates often have minor issues)
    cmd.add("-Awarnings")
    
    # Proc-macro crates need access to the proc_macro crate from sysroot
    if ctx.attrs.proc_macro:
        cmd.add("--extern", "proc_macro")
    
    # Features
    for feature in ctx.attrs.features:
        cmd.add("--cfg", 'feature="{}"'.format(feature))
    
    # Cfg flags
    for cfg in ctx.attrs.cfg:
        cmd.add("--cfg", cfg)
    
    # Rustc flags
    for flag in ctx.attrs.rustc_flags:
        cmd.add(flag)
    
    # Collect transitive deps for propagation (include our own output)
    transitive_deps = []
    
    # Collect dependency search paths and externs
    for dep in ctx.attrs.deps:
        if RustCrateInfo in dep:
            info = dep[RustCrateInfo]
            cmd.add(cmd_args("--extern", cmd_args(info.crate_name, "=", info.rlib, delimiter = "")))
            # Add the directory to -L so this dep can be found
            cmd.add(cmd_args(info.rlib, format = "-Ldependency={}", parent = 1))
            # Collect this dep's rlib for transitive propagation
            transitive_deps.append(info.rlib)
            # Also add all transitive deps' -L paths
            for trans_rlib in info.transitive_deps:
                cmd.add(cmd_args(trans_rlib, format = "-Ldependency={}", parent = 1))
                transitive_deps.append(trans_rlib)
    
    # Source directory from http_archive
    src_dir = ctx.attrs.src[DefaultInfo].default_outputs[0]
    
    # Crate root - path relative to extracted archive
    crate_root = ctx.attrs.crate_root or "src/lib.rs"
    cmd.add(cmd_args(src_dir, format = "{{}}/{}".format(crate_root)))
    
    # Build env dict for Cargo-like environment variables
    env = {}
    for key, val in ctx.attrs.env.items():
        env[key] = val
    
    # Handle generated files (for build.rs output simulation)
    if ctx.attrs.generated_files:
        generated_outputs = []
        for filename, content in ctx.attrs.generated_files.items():
            gen_file = ctx.actions.declare_output("generated/{}".format(filename))
            ctx.actions.write(gen_file, content)
            generated_outputs.append(gen_file)
        # Set OUT_DIR to the absolute directory containing generated files
        # The project root for Buck2 in this setup
        project_root = read_root_config("project", "root", ".")
        env["OUT_DIR"] = cmd_args(generated_outputs[0], parent = 1, format = project_root + "/{}")
        cmd.add(cmd_args(hidden = generated_outputs))
    
    ctx.actions.run(cmd, category = "rustc", identifier = crate_name, env = env)
    
    return [
        DefaultInfo(default_output = out),
        RustCrateInfo(
            rlib = out,
            rmeta = out,  # Use rlib as rmeta for simplicity
            crate_name = crate_name,
            edition = ctx.attrs.edition,
            features = ctx.attrs.features,
            is_proc_macro = ctx.attrs.proc_macro,
            transitive_deps = transitive_deps,
        ),
    ]

rust_crate = rule(
    impl = _rust_crate_impl,
    attrs = {
        "src": attrs.dep(),  # http_archive target
        "crate_name": attrs.option(attrs.string(), default = None),
        "crate_root": attrs.option(attrs.string(), default = None),
        "edition": attrs.string(default = "2021"),
        "features": attrs.list(attrs.string(), default = []),
        "cfg": attrs.list(attrs.string(), default = []),
        "deps": attrs.list(attrs.dep(), default = []),
        "proc_macro": attrs.bool(default = False),
        "rustc_flags": attrs.list(attrs.string(), default = []),
        "env": attrs.dict(attrs.string(), attrs.string(), default = {}),
        "generated_files": attrs.dict(attrs.string(), attrs.string(), default = {}),
    },
)

# Convenience macro to fetch and build a crate from crates.io
def crates_io(
    name: str,
    version: str,
    sha256: str,
    features: list[str] = [],
    deps: list[str] = [],
    proc_macro: bool = False,
    edition: str = "2021",
    crate_root: str | None = None,
    rustc_flags: list[str] = [],
    pkg_name: str | None = None,
    crate_name: str | None = None,
    env: dict[str, str] = {},
    generated_files: dict[str, str] = {},
    visibility: list[str] = ["PUBLIC"]):
    """
    Fetch and build a crate from crates.io.
    
    Args:
        name: Buck target name
        version: Crate version
        sha256: SHA256 of the crate tarball
        pkg_name: Package name on crates.io (defaults to name)
        crate_name: Crate name for rustc --extern (defaults to name with - replaced by _)
        env: Extra environment variables to set during build
    
    Example:
        crates_io(
            name = "serde",
            version = "1.0.228",
            sha256 = "abc123...",
            features = ["derive"],
            deps = [":serde_derive"],
        )
    """
    
    # pkg_name is the crates.io package name, defaults to target name
    pkg = pkg_name or name
    
    archive_name = "{}-{}.crate".format(name, version)
    
    # Fetch the crate
    http_archive(
        name = archive_name,
        urls = [_crate_url(pkg, version)],
        sha256 = sha256,
        strip_prefix = "{}-{}".format(pkg, version),
    )
    
    # Parse version for Cargo-like env vars
    version_parts = version.split(".")
    major = version_parts[0] if len(version_parts) > 0 else "0"
    minor = version_parts[1] if len(version_parts) > 1 else "0"
    patch_full = version_parts[2] if len(version_parts) > 2 else "0"
    # Handle pre-release suffixes like "1.0.25-alpha"
    patch = patch_full.split("-")[0].split("+")[0]
    
    # Generate Cargo-like environment variables
    cargo_env = {
        "CARGO_PKG_NAME": pkg,
        "CARGO_PKG_VERSION": version,
        "CARGO_PKG_VERSION_MAJOR": major,
        "CARGO_PKG_VERSION_MINOR": minor,
        "CARGO_PKG_VERSION_PATCH": patch,
    }
    # Merge with user-provided env (user env takes precedence)
    for k, v in env.items():
        cargo_env[k] = v
    
    # Build it
    rust_crate(
        name = name,
        src = ":{}".format(archive_name),
        crate_name = crate_name,
        edition = edition,
        features = features,
        deps = deps,
        proc_macro = proc_macro,
        crate_root = crate_root,
        rustc_flags = rustc_flags,
        env = cargo_env,
        generated_files = generated_files,
        visibility = visibility,
    )
