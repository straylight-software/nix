# toolchains/nix_analyze.bzl
#
# Nix integration for Buck2.
#
# Provides nix_library which dynamically resolves Nix dependencies using
# the nix-analyze tool. The output is a response file containing flags.
#
# Usage:
#   nix_library(name = "lib", flake_ref = "nixpkgs#lib")
#   cxx_binary(
#       name = "bin",
#       # Inject flags via response file expansion
#       preprocessor_flags = ["@$(location :lib)"],
#       linker_flags = ["@$(location :lib)"],
#   )

def _nix_library_impl(ctx):
    """
    Resolve a nix flake ref to compiler flags.
    Returns a response file containing the flags.
    """
    flake_ref = ctx.attrs.flake_ref
    
    flags_file = ctx.actions.declare_output("flags.rsp")
    analyzer = ctx.attrs._analyzer[RunInfo]
    
    cmd = cmd_args(
        analyzer,
        "resolve",
        flake_ref,
    )
    
    # nix-analyze resolve output is already formatted flags
    ctx.actions.run(
        cmd_args("sh", "-c", cmd_args(cmd, ">", flags_file.as_output(), delimiter=" ")),
        category = "nix_resolve",
        identifier = flake_ref,
    )
    
    return [
        DefaultInfo(default_output = flags_file),
    ]

nix_library = rule(
    impl = _nix_library_impl,
    attrs = {
        "flake_ref": attrs.string(),
        "_analyzer": attrs.exec_dep(default = "root//src/nix-analyze:nix-analyze"),
    },
)

def nix_cxx_binary(name, deps = [], preprocessor_flags = [], linker_flags = [], **kwargs):
    """
    Wrapper around cxx_binary that supports Nix flake references in 'deps'.
    
    Any dependency string containing '#' is treated as a Nix flake reference.
    It will be automatically resolved to compiler flags using nix-analyze.
    """
    real_deps = []
    nix_flags = []
    
    for dep in deps:
        # Check if it looks like a flake ref (has #)
        if type(dep) == "string" and "#" in dep:
            # Generate a unique name for this dependency target within this package
            # e.g. "mybin_nixpkgs_zlib"
            slug = dep.replace("#", "_").replace("/", "_").replace(".", "_")
            target_name = "{}_{}".format(name, slug)
            
            nix_library(
                name = target_name,
                flake_ref = dep,
            )
            
            # Add location macro to flags
            flag = "@$(location :{})".format(target_name)
            nix_flags.append(flag)
        else:
            real_deps.append(dep)
            
    native.cxx_binary(
        name = name,
        deps = real_deps,
        preprocessor_flags = nix_flags + preprocessor_flags,
        linker_flags = nix_flags + linker_flags,
        **kwargs
    )
