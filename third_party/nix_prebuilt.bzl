# third_party/nix_prebuilt.bzl
#
# Macro for prebuilt libraries from Nix store paths.
# Buck2 doesn't allow absolute paths in source attributes, so we use
# preprocessor and linker flags instead.

def nix_prebuilt_cxx_library(
        name,
        static_lib = None,
        shared_lib = None,
        include_dir = None,
        deps = [],
        exported_deps = [],
        exported_linker_flags = [],
        visibility = ["PUBLIC"]):
    """
    Prebuilt C++ library with paths from Nix store.

    Args:
        name: Target name
        static_lib: Absolute path to .a file
        shared_lib: Absolute path to .so file
        include_dir: Absolute path to include directory
        deps: Dependencies
        exported_deps: Exported dependencies (headers/flags propagated to dependents)
        exported_linker_flags: Additional linker flags (e.g. -lpthread)
        visibility: Visibility
    """
    exported_preprocessor_flags = []
    linker_flags = list(exported_linker_flags)  # copy to avoid mutating default

    if include_dir:
        exported_preprocessor_flags.append("-isystem" + include_dir)

    if static_lib:
        linker_flags.append(static_lib)
    elif shared_lib:
        # For shared libs, we need -L and -l flags
        # Extract directory and library name from path
        # e.g. /nix/store/.../lib/libfoo.so -> -L/nix/store/.../lib -lfoo
        linker_flags.append(shared_lib)

    native.cxx_library(
        name = name,
        exported_preprocessor_flags = exported_preprocessor_flags,
        exported_linker_flags = linker_flags,
        exported_deps = deps + exported_deps,
        visibility = visibility,
    )
