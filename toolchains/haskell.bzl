# toolchains/haskell.bzl
#
# Haskell toolchain and rules using GHC from Nix.
#
# Uses ghcWithPackages from the Nix devshell, which includes all
# dependencies. The bin/ghc wrapper filters Mercury-specific flags
# that stock GHC doesn't understand.
#
# Paths are read from .buckconfig.local [haskell] section.
#
# Rules:
#   haskell_toolchain  - toolchain definition
#   haskell_library    - compile to .hi/.o with HaskellLibraryInfo
#   haskell_binary     - executable from sources + deps
#   haskell_c_library  - FFI exports callable from C/C++
#   haskell_ffi_binary - Haskell calling C/C++ via FFI
#   haskell_script     - single-file scripts
#   haskell_test       - test executable

# NOTE: Must use upstream @prelude types for HaskellToolchainInfo since prelude
# haskell_binary rule expects that provider. Our custom rules (haskell_script,
# etc.) don't use the toolchain provider - they read config directly.
load("@prelude//haskell:toolchain.bzl", "HaskellToolchainInfo", "HaskellPlatformInfo")

# ═══════════════════════════════════════════════════════════════════════════════
# CONFIGURATION
# ═══════════════════════════════════════════════════════════════════════════════

# Mandatory compiler flags - applied to all Haskell compilation
# These are non-negotiable and cannot be overridden by targets
MANDATORY_GHC_FLAGS = [
    "-Wall",
    "-Werror",
]

def _get_ghc() -> str:
    return read_root_config("haskell", "ghc", "bin/ghc")

def _get_ghc_pkg() -> str:
    return read_root_config("haskell", "ghc_pkg", "bin/ghc-pkg")

def _get_package_db() -> str | None:
    return read_root_config("haskell", "global_package_db", None)

# ═══════════════════════════════════════════════════════════════════════════════
# PROVIDERS
# ═══════════════════════════════════════════════════════════════════════════════

HaskellLibraryInfo = provider(fields = {
    "package_name": provider_field(str),
    "hi_dir": provider_field(Artifact | None, default = None),
    "object_dir": provider_field(Artifact | None, default = None),
    "stub_dir": provider_field(Artifact | None, default = None),
    "hie_dir": provider_field(Artifact | None, default = None),  # For IDE support
    "objects": provider_field(list, default = []),
    "modules": provider_field(list, default = []),  # Source files for source-based deps
})

# For C consumers of Haskell FFI libraries
HaskellIncludeInfo = provider(fields = {
    "include_dirs": provider_field(list, default = []),
})

# ═══════════════════════════════════════════════════════════════════════════════
# TOOLCHAIN
# ═══════════════════════════════════════════════════════════════════════════════

def _haskell_toolchain_impl(ctx: AnalysisContext) -> list[Provider]:
    """
    Haskell toolchain with paths from .buckconfig.local.

    Reads [haskell] section for:
      ghc              - GHC compiler
      ghc_pkg          - GHC package manager
      haddock          - Documentation generator
      ghc_lib_dir      - GHC library directory
      global_package_db - Global package database
    """
    ghc = read_root_config("haskell", "ghc", "bin/ghc")
    ghc_pkg = read_root_config("haskell", "ghc_pkg", "bin/ghc-pkg")
    haddock = read_root_config("haskell", "haddock", "bin/haddock")

    return [
        DefaultInfo(),
        HaskellToolchainInfo(
            compiler = ghc,
            packager = ghc_pkg,
            linker = ghc,
            haddock = haddock,
            compiler_flags = ctx.attrs.compiler_flags,
            linker_flags = ctx.attrs.linker_flags,
            ghci_script_template = ctx.attrs.ghci_script_template,
            ghci_iserv_template = ctx.attrs.ghci_iserv_template,
            script_template_processor = ctx.attrs.script_template_processor,
            cache_links = True,
            archive_contents = "normal",
            support_expose_package = False,
        ),
        HaskellPlatformInfo(
            name = "x86_64-linux",
        ),
    ]

haskell_toolchain = rule(
    impl = _haskell_toolchain_impl,
    attrs = {
        "compiler_flags": attrs.list(attrs.string(), default = []),
        "linker_flags": attrs.list(attrs.string(), default = []),
        "ghci_script_template": attrs.option(attrs.source(), default = None),
        "ghci_iserv_template": attrs.option(attrs.source(), default = None),
        "script_template_processor": attrs.option(attrs.exec_dep(providers = [RunInfo]), default = None),
    },
    is_toolchain_rule = True,
)

# ═══════════════════════════════════════════════════════════════════════════════
# haskell_library - Compile to .hi/.o files
# ═══════════════════════════════════════════════════════════════════════════════

def _haskell_library_impl(ctx: AnalysisContext) -> list[Provider]:
    """
    Build a Haskell library.
    
    Compiles sources to .hi interface files and .o object files.
    For multi-source libraries, all sources are compiled together.
    """
    ghc = _get_ghc()
    package_db = _get_package_db()
    
    if not ctx.attrs.srcs:
        return [
            DefaultInfo(),
            HaskellLibraryInfo(package_name = ctx.attrs.name, modules = []),
        ]
    
    # Output directories
    obj_dir = ctx.actions.declare_output("objs", dir = True)
    hi_dir = ctx.actions.declare_output("hi", dir = True)
    stub_dir = ctx.actions.declare_output("stubs", dir = True)
    
    # Collect dependency hi directories for -i flag
    dep_hi_dirs = []
    dep_objects = []
    for dep in ctx.attrs.deps:
        if HaskellLibraryInfo in dep:
            lib_info = dep[HaskellLibraryInfo]
            if lib_info.hi_dir:
                dep_hi_dirs.append(lib_info.hi_dir)
            if lib_info.objects:
                dep_objects.extend(lib_info.objects)
            elif lib_info.object_dir:
                dep_objects.append(lib_info.object_dir)
    
    # Build GHC command
    cmd = cmd_args([ghc])
    cmd.add("-no-link")
    cmd.add("-package-env=-")
    
    if package_db:
        cmd.add("-package-db", package_db)
    
    cmd.add("-odir", obj_dir.as_output())
    cmd.add("-hidir", hi_dir.as_output())
    cmd.add("-stubdir", stub_dir.as_output())
    
    # Generate .hie files for IDE support (go-to-definition, etc.)
    hie_dir = ctx.actions.declare_output("hie", dir = True)
    cmd.add("-fwrite-ide-info")
    cmd.add("-hiedir", hie_dir.as_output())
    
    # Mandatory flags (non-negotiable)
    cmd.add(MANDATORY_GHC_FLAGS)
    
    # Language extensions
    cmd.add("-XGHC2024")
    for ext in ctx.attrs.language_extensions:
        cmd.add("-X{}".format(ext))
    
    # GHC options
    cmd.add(ctx.attrs.ghc_options)
    
    # Packages
    for pkg in ctx.attrs.packages:
        cmd.add("-package", pkg)
    
    # Include paths for dependencies
    for hi_d in dep_hi_dirs:
        cmd.add(cmd_args("-i", hi_d, delimiter = ""))
    
    # Sources
    cmd.add(ctx.attrs.srcs)
    
    ctx.actions.run(cmd, category = "haskell_compile", identifier = ctx.attrs.name)
    
    # Create static library from objects
    lib = ctx.actions.declare_output("lib{}.a".format(ctx.attrs.name))
    ar_cmd = cmd_args(
        "/bin/sh", "-c",
        cmd_args("ar rcs", lib.as_output(), cmd_args(obj_dir, format = "{}/*.o"), delimiter = " "),
    )
    ctx.actions.run(ar_cmd, category = "haskell_archive", identifier = ctx.attrs.name)
    
    return [
        DefaultInfo(
            default_output = lib,
            sub_targets = {
                "hi": [DefaultInfo(default_outputs = [hi_dir])],
                "stubs": [DefaultInfo(default_outputs = [stub_dir])],
                "objects": [DefaultInfo(default_outputs = [obj_dir])],
                "hie": [DefaultInfo(default_outputs = [hie_dir])],
            },
        ),
        HaskellLibraryInfo(
            package_name = ctx.attrs.name,
            hi_dir = hi_dir,
            object_dir = lib,
            stub_dir = stub_dir,
            hie_dir = hie_dir,
            objects = [],
            modules = ctx.attrs.srcs,
        ),
    ]

haskell_library = rule(
    impl = _haskell_library_impl,
    attrs = {
        "srcs": attrs.list(attrs.source(), default = []),
        "deps": attrs.list(attrs.dep(), default = []),
        "packages": attrs.list(attrs.string(), default = []),
        "ghc_options": attrs.list(attrs.string(), default = []),
        "language_extensions": attrs.list(attrs.string(), default = []),
    },
)

# ═══════════════════════════════════════════════════════════════════════════════
# haskell_binary - Executable from sources + deps
# ═══════════════════════════════════════════════════════════════════════════════

def _haskell_binary_impl(ctx: AnalysisContext) -> list[Provider]:
    """
    Build a Haskell executable.
    """
    ghc = _get_ghc()
    package_db = _get_package_db()
    
    out = ctx.actions.declare_output(ctx.attrs.name)
    
    # Output directories for intermediate files (keeps source tree clean)
    obj_dir = ctx.actions.declare_output("objs", dir = True)
    hi_dir = ctx.actions.declare_output("hi", dir = True)
    
    # Collect dependency info
    dep_hi_dirs = []
    dep_libs = []
    dep_sources = []  # For source-based deps
    for dep in ctx.attrs.deps:
        if HaskellLibraryInfo in dep:
            lib_info = dep[HaskellLibraryInfo]
            if lib_info.hi_dir:
                dep_hi_dirs.append(lib_info.hi_dir)
            if lib_info.objects:
                dep_libs.extend(lib_info.objects)
            elif lib_info.object_dir:
                dep_libs.append(lib_info.object_dir)
            # Also collect source modules for source-based compilation
            if lib_info.modules:
                dep_sources.extend(lib_info.modules)
    
    cmd = cmd_args([ghc])
    cmd.add("-package-env=-")
    cmd.add("-O2")
    
    # Output directories (intermediate .o/.hi files go to buck-out, not source tree)
    cmd.add("-odir", obj_dir.as_output())
    cmd.add("-hidir", hi_dir.as_output())
    
    # Generate .hie files for IDE support (go-to-definition, etc.)
    hie_dir = ctx.actions.declare_output("hie", dir = True)
    cmd.add("-fwrite-ide-info")
    cmd.add("-hiedir", hie_dir.as_output())
    

    # Mandatory flags (non-negotiable)
    cmd.add(MANDATORY_GHC_FLAGS)
    cmd.add("-XGHC2024")
    
    if package_db:
        cmd.add("-package-db", package_db)
    
    # Main module
    if ctx.attrs.main:
        cmd.add("-main-is", ctx.attrs.main)
    
    cmd.add("-o", out.as_output())
    
    # Language extensions
    for ext in ctx.attrs.language_extensions:
        cmd.add("-X{}".format(ext))
    
    # GHC options (includes compiler_flags for backwards compat)
    cmd.add(ctx.attrs.ghc_options)
    cmd.add(ctx.attrs.compiler_flags)
    
    # Packages
    for pkg in ctx.attrs.packages:
        cmd.add("-package", pkg)
    
    # Include paths for dependencies
    for hi_d in dep_hi_dirs:
        cmd.add(cmd_args("-i", hi_d, delimiter = ""))
    
    # Sources (our sources + source-based deps)
    cmd.add(ctx.attrs.srcs)
    cmd.add(dep_sources)
    
    # Link against compiled deps
    cmd.add(dep_libs)
    
    ctx.actions.run(cmd, category = "ghc", identifier = ctx.attrs.name)
    
    return [
        DefaultInfo(
            default_output = out,
            sub_targets = {
                "hi": [DefaultInfo(default_outputs = [hi_dir])],
                "hie": [DefaultInfo(default_outputs = [hie_dir])],
            },
        ),
        RunInfo(args = cmd_args(out)),
    ]

haskell_binary = rule(
    impl = _haskell_binary_impl,
    attrs = {
        "srcs": attrs.list(attrs.source()),
        "deps": attrs.list(attrs.dep(), default = []),
        "main": attrs.option(attrs.string(), default = None),
        "packages": attrs.list(attrs.string(), default = []),
        "ghc_options": attrs.list(attrs.string(), default = []),
        "language_extensions": attrs.list(attrs.string(), default = []),
        "compiler_flags": attrs.list(attrs.string(), default = []),  # Backwards compat
    },
)

# ═══════════════════════════════════════════════════════════════════════════════
# haskell_c_library - FFI exports callable from C/C++
# ═══════════════════════════════════════════════════════════════════════════════

def _haskell_c_library_impl(ctx: AnalysisContext) -> list[Provider]:
    """
    Build a C-callable library from Haskell code with foreign exports.
    
    Produces:
      1. Static library with Haskell code
      2. Stub headers for C consumers
      3. HaskellIncludeInfo for include path propagation
    
    C code must call hs_init() before any Haskell functions.
    """
    ghc = _get_ghc()
    package_db = _get_package_db()
    
    stub_dir = ctx.actions.declare_output("stubs", dir = True)
    lib = ctx.actions.declare_output("lib{}.a".format(ctx.attrs.name))
    
    # Collect dependency hi directories
    dep_hi_dirs = []
    for dep in ctx.attrs.deps:
        if HaskellLibraryInfo in dep:
            lib_info = dep[HaskellLibraryInfo]
            if lib_info.hi_dir:
                dep_hi_dirs.append(lib_info.hi_dir)
    
    # Compile each source individually to get proper stub generation
    objects = []
    hi_files = []
    
    for src in ctx.attrs.srcs:
        src_path = src.short_path
        if src_path.endswith(".hs"):
            base_name = src_path.replace(".hs", "").split("/")[-1]
            obj = ctx.actions.declare_output("{}.o".format(base_name))
            hi = ctx.actions.declare_output("{}.hi".format(base_name))
            
            cmd = cmd_args([ghc])
            cmd.add("-c")
            cmd.add("-package-env=-")
            cmd.add("-fPIC")  # Position independent for shared libs
            
            if package_db:
                cmd.add("-package-db", package_db)
            
            cmd.add("-stubdir", stub_dir.as_output())
            cmd.add("-o", obj.as_output())
            cmd.add("-ohi", hi.as_output())
            
            # Mandatory flags (non-negotiable)
            cmd.add(MANDATORY_GHC_FLAGS)
            
            # Language extensions (ForeignFunctionInterface is required)
            cmd.add("-XGHC2024")
            cmd.add("-XForeignFunctionInterface")
            for ext in ctx.attrs.language_extensions:
                cmd.add("-X{}".format(ext))
            
            cmd.add(ctx.attrs.ghc_options)
            
            # Dependencies
            for hi_d in dep_hi_dirs:
                cmd.add(cmd_args("-i", hi_d, delimiter = ""))
            
            for pkg in ctx.attrs.packages:
                cmd.add("-package", pkg)
            
            cmd.add(src)
            
            ctx.actions.run(cmd, category = "haskell_compile", identifier = src_path)
            objects.append(obj)
            hi_files.append(hi)
    
    if not objects:
        return [DefaultInfo()]
    
    # Create hi directory with symlinks
    hi_dir = ctx.actions.declare_output("hi", dir = True)
    hi_symlinks = {hi.basename: hi for hi in hi_files}
    ctx.actions.symlinked_dir(hi_dir, hi_symlinks)
    
    # Archive objects
    ar_cmd = cmd_args("ar", "rcs", lib.as_output())
    ar_cmd.add(objects)
    ctx.actions.run(ar_cmd, category = "haskell_archive", identifier = ctx.attrs.name)
    
    return [
        DefaultInfo(
            default_output = lib,
            sub_targets = {
                "stubs": [DefaultInfo(default_outputs = [stub_dir])],
                "hi": [DefaultInfo(default_outputs = hi_files)],
                "objects": [DefaultInfo(default_outputs = objects)],
            },
        ),
        HaskellIncludeInfo(include_dirs = [stub_dir]),
        HaskellLibraryInfo(
            package_name = ctx.attrs.name,
            hi_dir = hi_dir,
            object_dir = lib,
            stub_dir = stub_dir,
            objects = objects,
            modules = [],
        ),
    ]

haskell_c_library = rule(
    impl = _haskell_c_library_impl,
    attrs = {
        "srcs": attrs.list(attrs.source(), default = []),
        "deps": attrs.list(attrs.dep(), default = []),
        "packages": attrs.list(attrs.string(), default = ["base"]),
        "ghc_options": attrs.list(attrs.string(), default = []),
        "language_extensions": attrs.list(attrs.string(), default = []),
    },
    doc = """
    Build a C-callable static library from Haskell with foreign exports.
    
    Example Haskell:
        {-# LANGUAGE ForeignFunctionInterface #-}
        module FFI where
        foreign export ccall hs_double :: CInt -> IO CInt
        hs_double x = return (x * 2)
    
    Example C:
        #include "HsFFI.h"
        #include "FFI_stub.h"
        int main(int argc, char *argv[]) {
            hs_init(&argc, &argv);
            int result = hs_double(21);
            hs_exit();
            return 0;
        }
    """,
)

# ═══════════════════════════════════════════════════════════════════════════════
# haskell_ffi_binary - Haskell calling C/C++ via FFI
# ═══════════════════════════════════════════════════════════════════════════════

def _haskell_ffi_binary_impl(ctx: AnalysisContext) -> list[Provider]:
    """
    Build a Haskell binary that calls C/C++ code via FFI.
    
    Steps:
      1. Compile C++ sources to .o files with clang
      2. Compile and link Haskell sources with GHC, including the C++ objects
    """
    ghc = _get_ghc()
    cxx = read_root_config("cxx", "cxx", "clang++")
    
    # C++ stdlib paths for unwrapped clang
    gcc_include = read_root_config("cxx", "gcc_include", "")
    gcc_include_arch = read_root_config("cxx", "gcc_include_arch", "")
    glibc_include = read_root_config("cxx", "glibc_include", "")
    clang_resource_dir = read_root_config("cxx", "clang_resource_dir", "")
    gcc_lib_base = read_root_config("cxx", "gcc_lib_base", "")
    
    out = ctx.actions.declare_output(ctx.attrs.name)
    
    # Step 1: Compile C++ sources
    cxx_compile_flags = ["-std=c++17", "-O2", "-fPIC", "-c"]
    
    if gcc_include:
        cxx_compile_flags.extend(["-isystem", gcc_include])
    if gcc_include_arch:
        cxx_compile_flags.extend(["-isystem", gcc_include_arch])
    if glibc_include:
        cxx_compile_flags.extend(["-isystem", glibc_include])
    if clang_resource_dir:
        cxx_compile_flags.extend(["-resource-dir=" + clang_resource_dir])
    
    cxx_compile_flags.extend(["-I", "."])
    
    cxx_objects = []
    for src in ctx.attrs.cxx_srcs:
        obj_name = src.short_path.replace(".cpp", ".o").replace(".c", ".o")
        obj = ctx.actions.declare_output(obj_name)
        
        cmd = cmd_args([cxx] + cxx_compile_flags + ["-o", obj.as_output(), src])
        ctx.actions.run(cmd, category = "cxx_compile", identifier = src.short_path)
        cxx_objects.append(obj)
    
    # Step 2: Compile Haskell and link
    # Output directories for intermediate files (keeps source tree clean)
    obj_dir = ctx.actions.declare_output("hs_objs", dir = True)
    hi_dir = ctx.actions.declare_output("hs_hi", dir = True)
    
    ghc_cmd = cmd_args([ghc])
    ghc_cmd.add("-O2", "-threaded")
    
    # Output directories (intermediate .o/.hi files go to buck-out, not source tree)
    ghc_cmd.add("-odir", obj_dir.as_output())
    ghc_cmd.add("-hidir", hi_dir.as_output())
    
    # Mandatory flags (non-negotiable)
    ghc_cmd.add(MANDATORY_GHC_FLAGS)
    ghc_cmd.add("-XGHC2024")
    
    if gcc_lib_base:
        ghc_cmd.add("-optl", "-L" + gcc_lib_base)
    
    ghc_cmd.add("-lstdc++")
    ghc_cmd.add("-o", out.as_output())
    
    # Language extensions
    for ext in ctx.attrs.language_extensions:
        ghc_cmd.add("-X{}".format(ext))
    
    ghc_cmd.add(ctx.attrs.compiler_flags)
    ghc_cmd.add(ctx.attrs.hs_srcs)
    ghc_cmd.add(cxx_objects)
    
    ctx.actions.run(ghc_cmd, category = "ghc_link", identifier = ctx.attrs.name)
    
    return [
        DefaultInfo(default_output = out),
        RunInfo(args = [out]),
    ]

haskell_ffi_binary = rule(
    impl = _haskell_ffi_binary_impl,
    attrs = {
        "hs_srcs": attrs.list(attrs.source()),
        "cxx_srcs": attrs.list(attrs.source(), default = []),
        "cxx_headers": attrs.list(attrs.source(), default = []),
        "deps": attrs.list(attrs.dep(), default = []),
        "compiler_flags": attrs.list(attrs.string(), default = []),
        "language_extensions": attrs.list(attrs.string(), default = []),
    },
)

# ═══════════════════════════════════════════════════════════════════════════════
# haskell_script - Single-file scripts
# ═══════════════════════════════════════════════════════════════════════════════

def _haskell_script_impl(ctx: AnalysisContext) -> list[Provider]:
    """
    Build a single-file Haskell script.
    
    Uses ghcWithPackages from Nix for external deps.
    """
    ghc = _get_ghc()
    
    out = ctx.actions.declare_output(ctx.attrs.name)
    
    # Output directories for intermediate files (keeps source tree clean)
    obj_dir = ctx.actions.declare_output("objs", dir = True)
    hi_dir = ctx.actions.declare_output("hi", dir = True)
    
    cmd = cmd_args([ghc])
    
    # Output directories (intermediate .o/.hi files go to buck-out, not source tree)
    cmd.add("-odir", obj_dir.as_output())
    cmd.add("-hidir", hi_dir.as_output())
    
    # Mandatory flags (non-negotiable)
    cmd.add(MANDATORY_GHC_FLAGS)
    cmd.add("-XGHC2024")
    
    cmd.add(ctx.attrs.compiler_flags)
    cmd.add("-o", out.as_output())
    
    for include_path in ctx.attrs.include_paths:
        cmd.add("-i" + include_path)
    
    for pkg in ctx.attrs.packages:
        cmd.add("-package", pkg)
    
    cmd.add(ctx.attrs.srcs)
    
    ctx.actions.run(cmd, category = "haskell_script", identifier = ctx.attrs.name)
    
    return [
        DefaultInfo(default_output = out),
        RunInfo(args = [out]),
    ]

haskell_script = rule(
    impl = _haskell_script_impl,
    attrs = {
        "srcs": attrs.list(attrs.source()),
        "include_paths": attrs.list(attrs.string(), default = []),
        "compiler_flags": attrs.list(attrs.string(), default = []),
        "packages": attrs.list(attrs.string(), default = []),
    },
)

# ═══════════════════════════════════════════════════════════════════════════════
# haskell_test - Test executable (same as binary)
# ═══════════════════════════════════════════════════════════════════════════════

haskell_test = rule(
    impl = _haskell_binary_impl,
    attrs = {
        "srcs": attrs.list(attrs.source()),
        "deps": attrs.list(attrs.dep(), default = []),
        "main": attrs.option(attrs.string(), default = None),
        "packages": attrs.list(attrs.string(), default = ["base"]),
        "ghc_options": attrs.list(attrs.string(), default = []),
        "language_extensions": attrs.list(attrs.string(), default = []),
        "compiler_flags": attrs.list(attrs.string(), default = []),
    },
)
