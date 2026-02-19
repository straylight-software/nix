# toolchains/nix.bzl
#
# Rules for Nix flake dependencies in Buck2.
#
# Resolves nixpkgs#foo to store paths at analysis time via `nix build --print-out-paths`.
# The store paths are then used as prebuilt_cxx_library deps.
#
# Usage in BUCK:
#   load("@toolchains//:nix.bzl", "nix_prebuilt")
#
#   nix_prebuilt(
#       name = "zlib",
#       flake_ref = "nixpkgs#zlib",
#   )
#
#   cxx_binary(
#       name = "my-app",
#       srcs = ["main.cpp"],
#       deps = [":zlib"],
#   )

def _nix_prebuilt_impl(ctx: AnalysisContext) -> list[Provider]:
    """
    Resolve a nix flake ref to store paths and expose as prebuilt C library.
    """
    flake_ref = ctx.attrs.flake_ref
    
    # Read pre-resolved paths from config (set by nix develop shellHook)
    # Format in .buckconfig.local:
    #   [nix.resolved]
    #   nixpkgs#zlib.out = /nix/store/xxx-zlib-1.3.1
    #   nixpkgs#zlib.dev = /nix/store/xxx-zlib-1.3.1-dev
    
    out_path = read_root_config("nix.resolved", flake_ref + ".out", None)
    dev_path = read_root_config("nix.resolved", flake_ref + ".dev", None)
    
    if out_path == None:
        fail("Nix flake ref not resolved: {}. Run 'nix develop' to populate .buckconfig.local".format(flake_ref))
    
    # Use dev path for headers if available, otherwise out path
    include_path = (dev_path or out_path) + "/include"
    lib_path = out_path + "/lib"
    
    # The library name is usually the package name, but some differ
    pkg_name = flake_ref.split("#")[-1] if "#" in flake_ref else flake_ref
    lib_name = _lib_name_map.get(pkg_name, pkg_name)
    
    return [
        DefaultInfo(),
        # Expose as C++ library provider
        # Buck2 prelude expects specific providers for prebuilt libs
        # For now, just use raw compiler/linker flags
        CxxPrebuiltLibraryInfo(
            include_dirs = [include_path],
            lib_dir = lib_path,
            lib_name = lib_name,
        ),
    ]

# Map package names to actual library names
_lib_name_map = {
    "zlib": "z",
    "openssl": "ssl",
    "libpng": "png", 
    "libjpeg": "jpeg",
    "sqlite": "sqlite3",
}

# Simple provider to carry prebuilt lib info
CxxPrebuiltLibraryInfo = provider(fields = [
    "include_dirs",
    "lib_dir", 
    "lib_name",
])

nix_prebuilt = rule(
    impl = _nix_prebuilt_impl,
    attrs = {
        "flake_ref": attrs.string(),
    },
)

# Macro to generate prebuilt lib from flake ref
def nix_cxx_library(name: str, flake_ref: str, **kwargs):
    """
    Create a prebuilt_cxx_library from a nix flake reference.
    
    The actual store paths are resolved at analysis time from .buckconfig.local,
    which is populated by `nix develop` shell hook.
    """
    # Read resolved paths from config
    out_path = read_root_config("nix.resolved", flake_ref + ".out", None)
    dev_path = read_root_config("nix.resolved", flake_ref + ".dev", None)
    
    if out_path == None:
        # Can't fail at load time, so create a dummy target that fails at analysis
        nix_prebuilt(
            name = name,
            flake_ref = flake_ref,
        )
        return
    
    include_path = (dev_path or out_path) + "/include"
    lib_path = out_path + "/lib"
    
    pkg_name = flake_ref.split("#")[-1] if "#" in flake_ref else flake_ref
    lib_name = _lib_name_map.get(pkg_name, pkg_name)
    
    native.prebuilt_cxx_library(
        name = name,
        header_dirs = [include_path],
        static_lib = lib_path + "/lib" + lib_name + ".a",
        shared_lib = lib_path + "/lib" + lib_name + ".so",
        exported_preprocessor_flags = ["-isystem", include_path],
        exported_linker_flags = ["-L" + lib_path, "-l" + lib_name],
        **kwargs
    )
