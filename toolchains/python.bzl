# toolchains/python.bzl
#
# Python toolchain with nanobind for C++ bindings.
#
# Paths are read from .buckconfig.local [python] section.
# Uses Python from Nix devshell with nanobind pre-installed.
#
# For unwrapped clang, we need explicit stdlib include and library paths.
# Nanobind requires compiling its source files along with user code.

# Nanobind source files that must be compiled with the extension
NB_SOURCES = [
    "src/nb_internals.cpp",
    "src/nb_func.cpp",
    "src/nb_type.cpp",
    "src/nb_enum.cpp",
    "src/nb_ndarray.cpp",
    "src/nb_static_property.cpp",
    "src/nb_ft.cpp",
    "src/common.cpp",
    "src/error.cpp",
    "src/trampoline.cpp",
    "src/implicit.cpp",
]

def _python_script_impl(ctx: AnalysisContext) -> list[Provider]:
    """
    Python script rule that can depend on extension modules.
    
    Creates a wrapper script that sets PYTHONPATH to include extension modules.
    """
    interpreter = read_root_config("python", "interpreter", "python3")
    
    # Collect extension .so files from deps
    ext_outputs = []
    for dep in ctx.attrs.deps:
        info = dep[DefaultInfo]
        for out in info.default_outputs:
            if out.short_path.endswith(".so"):
                ext_outputs.append(out)
    
    if ext_outputs:
        # Create wrapper script that sets PYTHONPATH
        wrapper = ctx.actions.declare_output(ctx.attrs.name + "_run.sh")
        
        # Build wrapper content using cmd_args for proper artifact paths
        # Use delimiter="" to avoid newlines between args
        wrapper_cmd = cmd_args(delimiter = "")
        wrapper_cmd.add("#!/bin/bash\n")
        wrapper_cmd.add("# Auto-generated wrapper for " + ctx.attrs.name + "\n")
        # Find repo root by walking up to .buckconfig
        wrapper_cmd.add("ROOT=\"$(cd \"$(dirname \"$0\")\" && while [[ ! -f .buckconfig ]] && [[ $PWD != / ]]; do cd ..; done && pwd)\"\n")
        wrapper_cmd.add("export PYTHONPATH=\"$ROOT/")
        for i, ext in enumerate(ext_outputs):
            if i > 0:
                wrapper_cmd.add(":$ROOT/")
            # Use parent format to get directory of .so file
            wrapper_cmd.add(cmd_args(ext, parent = 1))
        wrapper_cmd.add("${PYTHONPATH:+:$PYTHONPATH}\"\n")
        wrapper_cmd.add("exec " + interpreter + " \"$ROOT/")
        wrapper_cmd.add(ctx.attrs.main)
        wrapper_cmd.add("\" \"$@\"\n")
        
        ctx.actions.write(wrapper, wrapper_cmd, is_executable = True)
        
        # The ext_outputs are tracked as hidden deps via cmd_args in wrapper_cmd
        # This ensures they get built before the wrapper can run
        return [
            DefaultInfo(default_output = wrapper, other_outputs = ext_outputs),
            RunInfo(args = cmd_args([wrapper], hidden = ext_outputs)),
        ]
    else:
        return [
            DefaultInfo(default_output = ctx.attrs.main),
            RunInfo(args = [interpreter, ctx.attrs.main]),
        ]

python_script = rule(
    impl = _python_script_impl,
    attrs = {
        "main": attrs.source(),
        "deps": attrs.list(attrs.dep(), default = []),
    },
)

def _nanobind_extension_impl(ctx: AnalysisContext) -> list[Provider]:
    """
    Build a nanobind C++ extension module.

    Uses clang from the :cxx toolchain and nanobind headers from Nix.
    Requires unwrapped clang with explicit stdlib include and library paths.
    Compiles nanobind source files along with user code.
    """
    # Get paths from config
    cxx = read_root_config("cxx", "cxx", "clang++")
    python_include = read_root_config("python", "python_include", "/usr/include/python3.12")
    nanobind_path = read_root_config("python", "nanobind_cmake", "")  # Package root with src/
    nanobind_include = read_root_config("python", "nanobind_include", "")

    # Stdlib paths for unwrapped clang
    gcc_include = read_root_config("cxx", "gcc_include", "")
    gcc_include_arch = read_root_config("cxx", "gcc_include_arch", "")
    glibc_include = read_root_config("cxx", "glibc_include", "")
    clang_resource_dir = read_root_config("cxx", "clang_resource_dir", "")

    # Library paths for linking
    gcc_lib = read_root_config("cxx", "gcc_lib", "")
    gcc_lib_base = read_root_config("cxx", "gcc_lib_base", "")
    glibc_lib = read_root_config("cxx", "glibc_lib", "")

    # Output .so file
    out = ctx.actions.declare_output(ctx.attrs.name + ".so")

    # Compile flags - must include stdlib paths for unwrapped clang
    compile_flags = [
        "-std=c++17",  # nanobind requires C++17
        "-O2",
        "-fPIC",
        "-shared",
        "-fvisibility=hidden",  # nanobind recommendation
        "-fno-strict-aliasing",
    ]

    # Add stdlib include paths (order matters for unwrapped clang)
    if gcc_include:
        compile_flags.extend(["-isystem", gcc_include])
    if gcc_include_arch:
        compile_flags.extend(["-isystem", gcc_include_arch])
    if glibc_include:
        compile_flags.extend(["-isystem", glibc_include])
    if clang_resource_dir:
        compile_flags.extend(["-resource-dir=" + clang_resource_dir])

    # Python and nanobind includes
    compile_flags.extend(["-isystem", python_include])
    if nanobind_include:
        compile_flags.extend(["-isystem", nanobind_include])

    # Nanobind's external dependencies (robin_map)
    if nanobind_path:
        compile_flags.extend(["-isystem", nanobind_path + "/ext/robin_map/include"])

    # Link flags for unwrapped clang
    # -B tells clang where to find CRT startup files (crti.o, crtbeginS.o, etc.)
    link_flags = []
    if glibc_lib:
        link_flags.extend(["-B" + glibc_lib, "-L" + glibc_lib])
    if gcc_lib:
        link_flags.extend(["-B" + gcc_lib, "-L" + gcc_lib])
    if gcc_lib_base:
        link_flags.extend(["-L" + gcc_lib_base])
    link_flags.extend(["-lstdc++", "-lm", "-ldl", "-lpthread"])

    # Collect all source files: user sources + nanobind sources
    all_srcs = [src for src in ctx.attrs.srcs]

    # Add nanobind source files if path is configured
    nb_srcs = []
    if nanobind_path:
        nb_srcs = [nanobind_path + "/" + src for src in NB_SOURCES]

    # Build command
    cmd = cmd_args([
        cxx,
    ] + compile_flags + link_flags + [
        "-o", out.as_output(),
    ] + all_srcs + nb_srcs)

    ctx.actions.run(cmd, category = "nanobind_compile")

    return [
        DefaultInfo(default_output = out),
    ]

nanobind_extension = rule(
    impl = _nanobind_extension_impl,
    attrs = {
        "srcs": attrs.list(attrs.source()),
        "deps": attrs.list(attrs.dep(), default = []),
        "compiler_flags": attrs.list(attrs.string(), default = []),
    },
)

# Import NvLibraryInfo from nv.bzl for type checking
load("@toolchains//:nv.bzl", "NvLibraryInfo")

def _pybind11_extension_impl(ctx: AnalysisContext) -> list[Provider]:
    """
    Build a pybind11 C++ extension module.
    
    pybind11 is header-only so simpler than nanobind.
    Supports nv_deps for linking CUDA libraries.
    """
    # Get paths from config
    cxx = read_root_config("cxx", "cxx", "clang++")
    python_include = read_root_config("python", "python_include", "/usr/include/python3.12")
    pybind11_include = read_root_config("python", "pybind11_include", "")

    # Stdlib paths for unwrapped clang
    gcc_include = read_root_config("cxx", "gcc_include", "")
    gcc_include_arch = read_root_config("cxx", "gcc_include_arch", "")
    glibc_include = read_root_config("cxx", "glibc_include", "")
    clang_resource_dir = read_root_config("cxx", "clang_resource_dir", "")

    # Library paths for linking
    gcc_lib = read_root_config("cxx", "gcc_lib", "")
    gcc_lib_base = read_root_config("cxx", "gcc_lib_base", "")
    glibc_lib = read_root_config("cxx", "glibc_lib", "")

    # NVIDIA SDK for CUDA dependencies
    nvidia_sdk_lib = read_root_config("nv", "nvidia_sdk_lib", "")
    nvidia_sdk_include = read_root_config("nv", "nvidia_sdk_include", "")

    # Output .so file
    out = ctx.actions.declare_output(ctx.attrs.name + ".so")

    # Compile flags
    compile_flags = [
        "-std=c++17",
        "-O2",
        "-fPIC",
        "-shared",
        "-fvisibility=hidden",
    ]

    # Add stdlib include paths
    if gcc_include:
        compile_flags.extend(["-isystem", gcc_include])
    if gcc_include_arch:
        compile_flags.extend(["-isystem", gcc_include_arch])
    if glibc_include:
        compile_flags.extend(["-isystem", glibc_include])
    if clang_resource_dir:
        compile_flags.extend(["-resource-dir=" + clang_resource_dir])

    # Python and pybind11 includes
    compile_flags.extend(["-isystem", python_include])
    if pybind11_include:
        compile_flags.extend(["-isystem", pybind11_include])

    # Add CUDA include if we have nv_deps
    if ctx.attrs.nv_deps and nvidia_sdk_include:
        compile_flags.extend(["-isystem", nvidia_sdk_include])

    # Collect objects and headers from nv_deps
    nv_objects = []
    nv_headers = []
    for dep in ctx.attrs.nv_deps:
        if NvLibraryInfo in dep:
            nv_info = dep[NvLibraryInfo]
            nv_objects.extend(nv_info.objects)
            nv_headers.extend(nv_info.headers)
            # Add include path for headers (use the source directory)
            if nv_info.headers:
                # Headers are source artifacts, add their parent dir to includes
                compile_flags.extend(["-I", "."])

    # Link flags
    # -B tells clang where to find CRT startup files (crti.o, crtbeginS.o, etc.)
    link_flags = []
    if glibc_lib:
        link_flags.extend(["-B" + glibc_lib, "-L" + glibc_lib])
    if gcc_lib:
        link_flags.extend(["-B" + gcc_lib, "-L" + gcc_lib])
    if gcc_lib_base:
        link_flags.extend(["-L" + gcc_lib_base])
    link_flags.extend(["-lstdc++", "-lm", "-ldl", "-lpthread"])

    # Add CUDA runtime library if we have nv_deps
    if ctx.attrs.nv_deps and nvidia_sdk_lib:
        link_flags.extend([
            "-L" + nvidia_sdk_lib,
            "-Wl,-rpath," + nvidia_sdk_lib,
            "-lcudart",
        ])

    # Build command: compile user sources and link with nv objects
    cmd = cmd_args([
        cxx,
    ] + compile_flags + link_flags + [
        "-o", out.as_output(),
    ] + [src for src in ctx.attrs.srcs] + nv_objects)

    ctx.actions.run(cmd, category = "pybind11_compile")

    return [
        DefaultInfo(default_output = out),
    ]

pybind11_extension = rule(
    impl = _pybind11_extension_impl,
    attrs = {
        "srcs": attrs.list(attrs.source()),
        "deps": attrs.list(attrs.dep(), default = []),
        "nv_deps": attrs.list(attrs.dep(), default = []),
        "compiler_flags": attrs.list(attrs.string(), default = []),
    },
)
