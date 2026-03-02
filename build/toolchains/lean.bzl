# lean.bzl
# Lean 4 compilation rules for Buck2 with Nix toolchain integration
#
# Lean 4 compiles to C, which we then compile with our C++ toolchain.
# This enables proof-carrying code: Lean theorems constrain generated C,
# which links into Rust/Haskell/Python via FFI.
#
# Key features:
#   - lean_library: Build a Lean library (.olean files + C extraction)
#   - lean_binary: Build a Lean executable
#   - lean_c_library: Extract C code from Lean for FFI linking
#
# Configuration (in .buckconfig):
#   [lean]
#   lean = /path/to/lean           # Lean compiler
#   leanc = /path/to/leanc         # Lean C compiler wrapper
#   lean_lib_dir = /path/to/lib    # Lean standard library
#   lean_include_dir = /path/to/include  # Lean C headers
#
# Usage:
#   lean_library(
#       name = "proofs",
#       srcs = ["VillaStraylight.lean"],
#       deps = ["//lib:mathlib"],
#   )
#
#   lean_c_library(
#       name = "verified_kernel",
#       srcs = ["Kernel.lean"],
#       deps = [":proofs"],
#       # Extracted C code can be linked into cxx_library
#   )
#
# The proof-carrying pattern:
#   1. Write theorems in Lean (compile-time verification)
#   2. Extract to C via lean_c_library
#   3. Link C into Rust/C++ via FFI
#   4. Fuzz at runtime against Lean specs (omega tactic IRL)

# ═══════════════════════════════════════════════════════════════════════════════
# PROVIDERS
# ═══════════════════════════════════════════════════════════════════════════════

LeanLibraryInfo = provider(fields = {
    "olean_dir": provider_field(Artifact | None, default = None),
    "c_dir": provider_field(Artifact | None, default = None),
    "lib_name": provider_field(str, default = ""),
    "deps": provider_field(list, default = []),
})

LeanCLibraryInfo = provider(fields = {
    "c_sources": provider_field(list[Artifact], default = []),
    "include_dir": provider_field(Artifact | None, default = None),
    "objects": provider_field(list[Artifact], default = []),
    "archive": provider_field(Artifact | None, default = None),
})

# ═══════════════════════════════════════════════════════════════════════════════
# CONFIGURATION
# ═══════════════════════════════════════════════════════════════════════════════

def _get_lean() -> str:
    """Get lean compiler path from config."""
    path = read_root_config("lean", "lean", None)
    if path == None:
        fail("""
lean compiler not configured.

Configure your toolchain via Nix:

    [lean]
    lean = /nix/store/.../bin/lean
    leanc = /nix/store/.../bin/leanc
    lean_lib_dir = /nix/store/.../lib/lean
    lean_include_dir = /nix/store/.../include

Then run: nix develop
""")
    return path

def _get_leanc() -> str:
    """Get leanc (Lean C compiler wrapper) path from config."""
    path = read_root_config("lean", "leanc", None)
    if path == None:
        fail("leanc not configured. See [lean] section in .buckconfig")
    return path

def _get_lean_lib_dir() -> str | None:
    """Get Lean standard library directory."""
    return read_root_config("lean", "lean_lib_dir", None)

def _get_lean_include_dir() -> str | None:
    """Get Lean C headers directory."""
    return read_root_config("lean", "lean_include_dir", None)

# ═══════════════════════════════════════════════════════════════════════════════
# LEAN LIBRARY RULE
# ═══════════════════════════════════════════════════════════════════════════════

def _lean_library_impl(ctx: AnalysisContext) -> list[Provider]:
    """
    Build a Lean library.
    
    Compiles Lean source files to .olean (compiled interface) files.
    Optionally extracts C code for FFI.
    
    Lean compilation model:
    - Each .lean file compiles to .olean (binary interface)
    - Dependencies must be compiled first (no circular deps)
    - C extraction happens via --c flag
    """
    lean = _get_lean()
    lean_lib_dir = _get_lean_lib_dir()
    
    if not ctx.attrs.srcs:
        return [DefaultInfo(), LeanLibraryInfo()]
    
    # Output directories
    olean_dir = ctx.actions.declare_output("olean", dir = True)
    c_dir = ctx.actions.declare_output("c", dir = True) if ctx.attrs.extract_c else None
    
    # Collect dependency olean directories
    dep_paths = []
    for dep in ctx.attrs.deps:
        if LeanLibraryInfo in dep:
            info = dep[LeanLibraryInfo]
            if info.olean_dir:
                dep_paths.append(info.olean_dir)
    
    # Build script
    script_parts = ["set -e"]
    script_parts.append("mkdir -p $OLEAN_DIR")
    if c_dir:
        script_parts.append("mkdir -p $C_DIR")
    
    # Build LEAN_PATH from dependencies and stdlib
    lean_path_parts = ["$OLEAN_DIR"]
    if lean_lib_dir:
        lean_path_parts.append(lean_lib_dir)
    for dep_path in dep_paths:
        lean_path_parts.append(cmd_args(dep_path))
    
    script_parts.append(cmd_args(
        "export LEAN_PATH=",
        cmd_args(lean_path_parts, delimiter = ":"),
        delimiter = "",
    ))
    
    # Compile each source file
    # Lean requires sources to be in --root directory, so we copy to scratch
    for src in ctx.attrs.srcs:
        # Module name from filename (Foo/Bar.lean -> Foo.Bar)
        # Simplified: just use basename without extension for now
        module_name = src.basename.removesuffix(".lean")
        
        # Copy source to scratch dir (Lean's --root requirement)
        script_parts.append(cmd_args("cp", src, "$BUCK_SCRATCH_PATH/", delimiter = " "))
        
        compile_cmd = [lean, "--root=$BUCK_SCRATCH_PATH"]
        compile_cmd.extend(ctx.attrs.lean_flags)
        compile_cmd.extend(["-o", cmd_args("$OLEAN_DIR/", module_name, ".olean", delimiter = "")])
        
        if c_dir:
            compile_cmd.append(cmd_args("--c=$C_DIR/", module_name, ".c", delimiter = ""))
        
        compile_cmd.append("$BUCK_SCRATCH_PATH/{}".format(src.basename))
        
        script_parts.append(cmd_args(compile_cmd, delimiter = " "))
    
    # Assemble full command
    script = cmd_args(script_parts, delimiter = "\n")
    
    outputs = [olean_dir.as_output()]
    env_parts = ["OLEAN_DIR=", olean_dir.as_output()]
    
    if c_dir:
        outputs.append(c_dir.as_output())
        env_parts.extend([" C_DIR=", c_dir.as_output()])
    
    cmd = cmd_args(
        "/bin/sh", "-c",
        cmd_args(env_parts, " && ", script, delimiter = ""),
    )
    
    # Hidden inputs for dependency tracking
    hidden = list(ctx.attrs.srcs)
    for dep_path in dep_paths:
        hidden.append(dep_path)
    
    ctx.actions.run(
        cmd_args(cmd, hidden = hidden),
        category = "lean_compile",
        identifier = ctx.attrs.name,
        local_only = True,  # Lean compilation needs consistent LEAN_PATH
    )
    
    return [
        DefaultInfo(
            default_output = olean_dir,
            sub_targets = {
                "olean": [DefaultInfo(default_outputs = [olean_dir])],
            } | ({"c": [DefaultInfo(default_outputs = [c_dir])]} if c_dir else {}),
        ),
        LeanLibraryInfo(
            olean_dir = olean_dir,
            c_dir = c_dir,
            lib_name = ctx.attrs.name,
            deps = ctx.attrs.deps,
        ),
    ]

lean_library = rule(
    impl = _lean_library_impl,
    attrs = {
        "srcs": attrs.list(attrs.source(), default = [], doc = "Lean source files (.lean)"),
        "deps": attrs.list(attrs.dep(), default = [], doc = "Lean library dependencies"),
        "lean_flags": attrs.list(attrs.string(), default = [], doc = "Additional lean compiler flags"),
        "extract_c": attrs.bool(default = False, doc = "Extract C code for FFI"),
    },
)

# ═══════════════════════════════════════════════════════════════════════════════
# LEAN BINARY RULE
# ═══════════════════════════════════════════════════════════════════════════════

def _lean_binary_impl(ctx: AnalysisContext) -> list[Provider]:
    """
    Build a Lean executable.
    
    Compiles Lean sources and links into an executable using leanc.
    
    Supports hierarchical module structure via root_module attr:
      - root_module = "Straylight" means files map to Straylight.Foo, Straylight.Bar
      - Sources are copied preserving structure under $ROOT/Straylight/
      - Main.lean is compiled last and linked
    
    For flat projects (no root_module), files compile as top-level modules.
    """
    lean = _get_lean()
    leanc = _get_leanc()
    lean_lib_dir = _get_lean_lib_dir()
    
    if not ctx.attrs.srcs:
        fail("lean_binary requires at least one source file")
    
    # Output
    exe = ctx.actions.declare_output(ctx.attrs.name)
    olean_dir = ctx.actions.declare_output("olean", dir = True)
    c_dir = ctx.actions.declare_output("c", dir = True)
    
    # Collect dependency olean directories
    dep_paths = []
    dep_c_dirs = []
    for dep in ctx.attrs.deps:
        if LeanLibraryInfo in dep:
            info = dep[LeanLibraryInfo]
            if info.olean_dir:
                dep_paths.append(info.olean_dir)
            if info.c_dir:
                dep_c_dirs.append(info.c_dir)
    
    # Build LEAN_PATH - include scratch dir for local modules
    lean_path_parts = ["$OLEAN_DIR", "$BUCK_SCRATCH_PATH"]
    if lean_lib_dir:
        lean_path_parts.append(lean_lib_dir)
    for dep_path in dep_paths:
        lean_path_parts.append(cmd_args(dep_path))
    
    # Script: setup, compile to C, then link
    script_parts = ["set -e"]
    script_parts.append("mkdir -p $OLEAN_DIR $C_DIR")
    
    script_parts.append(cmd_args(
        "export LEAN_PATH=",
        cmd_args(lean_path_parts, delimiter = ":"),
        delimiter = "",
    ))
    
    # Determine module structure
    root_module = ctx.attrs.root_module
    
    # Copy sources to scratch with proper structure
    # For hierarchical modules: Foo.lean -> $SCRATCH/RootModule/Foo.lean
    # For flat modules: Foo.lean -> $SCRATCH/Foo.lean
    c_files = []
    compile_order = []
    main_src = None
    
    for src in ctx.attrs.srcs:
        if src.basename == "Main.lean":
            main_src = src
        else:
            compile_order.append(src)
    
    # Main.lean must be compiled last
    if main_src:
        compile_order.append(main_src)
    else:
        # No Main.lean, use first source as main
        main_src = ctx.attrs.srcs[0]
    
    # Setup scratch directory structure
    if root_module:
        script_parts.append("mkdir -p $BUCK_SCRATCH_PATH/{}".format(root_module))
    
    # Copy and compile each source
    for src in compile_order:
        module_name = src.basename.removesuffix(".lean")
        
        if root_module and src.basename != "Main.lean":
            # Hierarchical: copy to RootModule/Foo.lean
            dest_path = "$BUCK_SCRATCH_PATH/{}/{}".format(root_module, src.basename)
            full_module = "{}.{}".format(root_module, module_name)
            c_file = "$C_DIR/{}.{}.c".format(root_module, module_name)
            olean_file = "$OLEAN_DIR/{}/{}.olean".format(root_module, module_name)
            script_parts.append("mkdir -p $OLEAN_DIR/{}".format(root_module))
        else:
            # Flat: copy to Foo.lean or Main.lean at root
            dest_path = "$BUCK_SCRATCH_PATH/{}".format(src.basename)
            full_module = module_name
            c_file = "$C_DIR/{}.c".format(module_name)
            olean_file = "$OLEAN_DIR/{}.olean".format(module_name)
        
        c_files.append(c_file)
        
        # Copy source
        script_parts.append(cmd_args("cp", src, dest_path, delimiter = " "))
        
        # Compile
        compile_cmd = [
            lean,
            "--root=$BUCK_SCRATCH_PATH",
            "-o", olean_file,
            "--c={}".format(c_file),
        ]
        compile_cmd.extend(ctx.attrs.lean_flags)
        compile_cmd.append(dest_path)
        
        script_parts.append(cmd_args(compile_cmd, delimiter = " "))
    
    # Link with leanc
    link_cmd = [leanc, "-o", exe.as_output()]
    link_cmd.extend(ctx.attrs.link_flags)
    
    # Add all C files
    for c_file in c_files:
        link_cmd.append(c_file)
    
    # Add dependency C files
    for dep_c_dir in dep_c_dirs:
        link_cmd.append(cmd_args(dep_c_dir, "/*.c", delimiter = ""))
    
    script_parts.append(cmd_args(link_cmd, delimiter = " "))
    
    script = cmd_args(script_parts, delimiter = "\n")
    
    cmd = cmd_args(
        "/bin/sh", "-c",
        cmd_args(
            "OLEAN_DIR=", olean_dir.as_output(),
            " C_DIR=", c_dir.as_output(),
            " && ", script,
            delimiter = "",
        ),
    )
    
    hidden = list(ctx.attrs.srcs)
    hidden.extend(dep_paths)
    hidden.extend(dep_c_dirs)
    
    ctx.actions.run(
        cmd_args(cmd, hidden = hidden),
        category = "lean_link",
        identifier = ctx.attrs.name,
        local_only = True,
    )
    
    return [
        DefaultInfo(default_output = exe),
        RunInfo(args = cmd_args(exe)),
    ]

lean_binary = rule(
    impl = _lean_binary_impl,
    attrs = {
        "srcs": attrs.list(attrs.source(), default = [], doc = "Lean source files (.lean)"),
        "deps": attrs.list(attrs.dep(), default = [], doc = "Lean library dependencies"),
        "root_module": attrs.option(attrs.string(), default = None, doc = "Root module name for hierarchical imports (e.g., 'Straylight')"),
        "lean_flags": attrs.list(attrs.string(), default = [], doc = "Additional lean compiler flags"),
        "link_flags": attrs.list(attrs.string(), default = [], doc = "Additional linker flags"),
    },
)

# ═══════════════════════════════════════════════════════════════════════════════
# LEAN C LIBRARY RULE
# ═══════════════════════════════════════════════════════════════════════════════

def _lean_c_library_impl(ctx: AnalysisContext) -> list[Provider]:
    """
    Extract C code from Lean for FFI linking.
    
    This is the proof-carrying code pattern:
    1. Theorems are proven in Lean (compile-time)
    2. C code is extracted (guaranteed to satisfy the theorems)
    3. C code links into Rust/C++/Haskell via FFI
    
    The extracted C can be used as a cxx_library dependency.
    
    **Relevant for computational linear algebra:**
    This is how Villa Straylight's layout proofs become callable from CUDA kernels.
    The Lean theorem (e.g., FTTC) constrains the generated C types.
    """
    lean = _get_lean()
    lean_include_dir = _get_lean_include_dir()
    lean_lib_dir = _get_lean_lib_dir()
    
    if not ctx.attrs.srcs:
        return [DefaultInfo(), LeanCLibraryInfo()]
    
    # Outputs
    c_dir = ctx.actions.declare_output("c", dir = True)
    olean_dir = ctx.actions.declare_output("olean", dir = True)
    include_dir = ctx.actions.declare_output("include", dir = True)
    obj_dir = ctx.actions.declare_output("obj", dir = True)
    archive = ctx.actions.declare_output("lib{}.a".format(ctx.attrs.name))
    
    # Collect dependencies
    dep_paths = []
    for dep in ctx.attrs.deps:
        if LeanLibraryInfo in dep:
            info = dep[LeanLibraryInfo]
            if info.olean_dir:
                dep_paths.append(info.olean_dir)
    
    # Build LEAN_PATH - use env var since olean_dir is output
    lean_path_parts = ["$OLEAN_DIR"]
    if lean_lib_dir:
        lean_path_parts.append(lean_lib_dir)
    for dep_path in dep_paths:
        lean_path_parts.append(cmd_args(dep_path))
    
    # Get C compiler from cxx config (we use our Clang, not leanc's default)
    cc = read_root_config("cxx", "cxx", "clang++")
    
    script_parts = ["set -e"]
    script_parts.append("mkdir -p $OLEAN_DIR $C_DIR $INCLUDE_DIR $OBJ_DIR")
    
    script_parts.append(cmd_args(
        "export LEAN_PATH=",
        cmd_args(lean_path_parts, delimiter = ":"),
        delimiter = "",
    ))
    
    # Compile Lean to C
    # Lean requires sources to be in --root directory, so we copy to scratch
    c_files = []
    for src in ctx.attrs.srcs:
        module_name = src.basename.removesuffix(".lean")
        c_file = "{}.c".format(module_name)
        c_files.append(c_file)
        
        # Copy source to scratch dir (Lean's --root requirement)
        script_parts.append(cmd_args("cp", src, "$BUCK_SCRATCH_PATH/", delimiter = " "))
        
        compile_cmd = [
            lean,
            "--root=$BUCK_SCRATCH_PATH",
            "-o", "$OLEAN_DIR/{}.olean".format(module_name),
            "--c=$C_DIR/{}".format(c_file),
        ]
        compile_cmd.extend(ctx.attrs.lean_flags)
        compile_cmd.append("$BUCK_SCRATCH_PATH/{}".format(src.basename))
        
        script_parts.append(cmd_args(compile_cmd, delimiter = " "))
    
    # Generate header file for FFI exports
    # Lean generates lean.h style headers; we create a wrapper
    header_content = [
        "// Generated by lean_c_library: {}".format(ctx.attrs.name),
        "#pragma once",
        "#include <lean/lean.h>",
        "",
        "// Exported functions from Lean",
    ]
    for export in ctx.attrs.exports:
        header_content.append("extern lean_object* {}(lean_object*);".format(export))
    
    script_parts.append(cmd_args(
        "cat > $INCLUDE_DIR/{}.h << 'LEAN_HEADER_EOF'\n{}\nLEAN_HEADER_EOF".format(
            ctx.attrs.name,
            "\n".join(header_content),
        ),
    ))
    
    # Compile C to objects
    for c_file in c_files:
        obj_file = c_file.removesuffix(".c") + ".o"
        
        cc_cmd = [cc, "-c", "-O2", "-fPIC"]
        if lean_include_dir:
            cc_cmd.extend(["-I", lean_include_dir])
        cc_cmd.extend(["-I", "$INCLUDE_DIR"])
        cc_cmd.extend(ctx.attrs.cflags)
        cc_cmd.extend(["-o", "$OBJ_DIR/{}".format(obj_file)])
        cc_cmd.append("$C_DIR/{}".format(c_file))
        
        script_parts.append(cmd_args(cc_cmd, delimiter = " "))
    
    # Archive objects
    script_parts.append(cmd_args(
        "ar rcs", archive.as_output(), "$OBJ_DIR/*.o",
        delimiter = " ",
    ))
    
    script = cmd_args(script_parts, delimiter = "\n")
    
    cmd = cmd_args(
        "/bin/sh", "-c",
        cmd_args(
            "OLEAN_DIR=", olean_dir.as_output(),
            " C_DIR=", c_dir.as_output(),
            " INCLUDE_DIR=", include_dir.as_output(),
            " OBJ_DIR=", obj_dir.as_output(),
            " && ", script,
            delimiter = "",
        ),
    )
    
    hidden = list(ctx.attrs.srcs)
    hidden.extend(dep_paths)
    
    ctx.actions.run(
        cmd_args(cmd, hidden = hidden),
        category = "lean_c_extract",
        identifier = ctx.attrs.name,
        local_only = True,
    )
    
    return [
        DefaultInfo(
            default_output = archive,
            sub_targets = {
                "c": [DefaultInfo(default_outputs = [c_dir])],
                "include": [DefaultInfo(default_outputs = [include_dir])],
                "olean": [DefaultInfo(default_outputs = [olean_dir])],
            },
        ),
        LeanLibraryInfo(
            olean_dir = olean_dir,
            c_dir = c_dir,
            lib_name = ctx.attrs.name,
            deps = ctx.attrs.deps,
        ),
        LeanCLibraryInfo(
            c_sources = [],  # We don't track individual files in dir output
            include_dir = include_dir,
            objects = [],
            archive = archive,
        ),
    ]

lean_c_library = rule(
    impl = _lean_c_library_impl,
    attrs = {
        "srcs": attrs.list(attrs.source(), default = [], doc = "Lean source files (.lean)"),
        "deps": attrs.list(attrs.dep(), default = [], doc = "Lean library dependencies"),
        "lean_flags": attrs.list(attrs.string(), default = [], doc = "Additional lean compiler flags"),
        "cflags": attrs.list(attrs.string(), default = [], doc = "Additional C compiler flags"),
        "exports": attrs.list(attrs.string(), default = [], doc = "Functions to export in header"),
    },
)

# ═══════════════════════════════════════════════════════════════════════════════
# LEAN TOOLCHAIN RULE
# ═══════════════════════════════════════════════════════════════════════════════

LeanToolchainInfo = provider(fields = {
    "lean": provider_field(str),
    "leanc": provider_field(str),
    "lean_lib_dir": provider_field(str | None, default = None),
    "lean_include_dir": provider_field(str | None, default = None),
})

def _lean_toolchain_impl(ctx: AnalysisContext) -> list[Provider]:
    """
    Lean toolchain with paths from .buckconfig.local.

    Reads [lean] section for absolute Nix store paths:
      lean           - Lean compiler
      leanc          - Lean C code generator
      lean_lib_dir   - Lean library directory
      lean_include_dir - Lean include directory
    """
    # Read from config, fall back to attrs
    lean = read_root_config("lean", "lean", ctx.attrs.lean)
    leanc = read_root_config("lean", "leanc", ctx.attrs.leanc)
    lean_lib_dir = read_root_config("lean", "lean_lib_dir", ctx.attrs.lean_lib_dir)
    lean_include_dir = read_root_config("lean", "lean_include_dir", ctx.attrs.lean_include_dir)

    return [
        DefaultInfo(),
        LeanToolchainInfo(
            lean = lean,
            leanc = leanc,
            lean_lib_dir = lean_lib_dir,
            lean_include_dir = lean_include_dir,
        ),
    ]

lean_toolchain = rule(
    impl = _lean_toolchain_impl,
    attrs = {
        "lean": attrs.string(default = "lean", doc = "Path to lean compiler"),
        "leanc": attrs.string(default = "leanc", doc = "Path to leanc"),
        "lean_lib_dir": attrs.option(attrs.string(), default = None, doc = "Lean library directory"),
        "lean_include_dir": attrs.option(attrs.string(), default = None, doc = "Lean include directory"),
    },
    is_toolchain_rule = True,
)

def _system_lean_toolchain_impl(_ctx: AnalysisContext) -> list[Provider]:
    """
    System Lean toolchain.
    
    DISABLED. No fallbacks. Configure via Nix or fail.
    """
    fail("""
system_lean_toolchain is disabled.

Zeitschrift does not support fallback toolchains.
Configure your Lean toolchain via Nix:

    [lean]
    lean = /nix/store/.../bin/lean
    leanc = /nix/store/.../bin/leanc
    lean_lib_dir = /nix/store/.../lib/lean
    lean_include_dir = /nix/store/.../include

Then run: nix develop

If you see this error, your .buckconfig.local is missing or stale.
""")

system_lean_toolchain = rule(
    impl = _system_lean_toolchain_impl,
    attrs = {},
    is_toolchain_rule = True,
)

# ═══════════════════════════════════════════════════════════════════════════════
# LEAN LAKE PROJECT RULE - DISABLED
# ═══════════════════════════════════════════════════════════════════════════════
#
# Lake is Lean's package manager but introduces non-hermetic builds:
#   - Downloads ~2GB for Mathlib
#   - Caches in .lake/ outside Buck2's control
#   - No content-addressed derivations
#
# If you need Mathlib, manage it via Nix overlays or a separate toolchain.
# The lean_library/lean_binary rules work for standalone Lean code.

def _lean_lake_build_impl(ctx: AnalysisContext) -> list[Provider]:
    """DISABLED: Lake builds are non-hermetic."""
    fail("""
lean_lake_build is disabled.

Lake introduces non-hermetic builds that bypass Buck2's caching and
content-addressed derivation model. Mathlib downloads (~2GB) and
.lake/ caches are not tracked.

Options:
  1. Use lean_library/lean_binary for standalone Lean code (no Lake deps)
  2. Manage Mathlib via Nix overlay (recommended for large proofs)
  3. Build outside Buck2 with 'lake build' directly

See: toolchains/lean.bzl for lean_library, lean_binary, lean_c_library
""")

lean_lake_build = rule(
    impl = _lean_lake_build_impl,
    attrs = {
        "srcs": attrs.list(attrs.source(), default = [], doc = "Lean source files"),
        "lakefile": attrs.option(attrs.source(), default = None, doc = "lakefile.lean"),
        "toolchain_file": attrs.option(attrs.source(), default = None, doc = "lean-toolchain file"),
    },
)
