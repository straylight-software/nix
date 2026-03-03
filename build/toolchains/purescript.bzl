# toolchains/purescript.bzl
#
# PureScript compilation rules for Buck2 with Nix toolchain integration.
#
# PureScript compiles to JavaScript using spago for dependency management.
# Halogen and other packages are fetched from the PureScript registry.
#
# Key features:
#   - purescript_library: Build a PureScript library
#   - purescript_binary: Build a PureScript web application (with spago)
#   - purescript_app: Build a Halogen/web app with HTML entry point
#
# Configuration (in .buckconfig):
#   [purescript]
#   purs = /path/to/purs           # PureScript compiler
#   spago = /path/to/spago         # Spago package manager
#   node = /path/to/node           # Node.js runtime
#
# Usage:
#   purescript_app(
#       name = "myapp",
#       srcs = glob(["src/**/*.purs"]),
#       spago_yaml = "spago.yaml",
#       index_html = "index.html",
#   )

# ═══════════════════════════════════════════════════════════════════════════════
# PROVIDERS
# ═══════════════════════════════════════════════════════════════════════════════

PureScriptLibraryInfo = provider(fields = {
    "output_dir": provider_field(Artifact | None, default = None),
    "lib_name": provider_field(str, default = ""),
    "deps": provider_field(list, default = []),
})

# ═══════════════════════════════════════════════════════════════════════════════
# CONFIGURATION
# ═══════════════════════════════════════════════════════════════════════════════

def _get_purs() -> str:
    """Get purs compiler path from config."""
    path = read_root_config("purescript", "purs", None)
    if path == None:
        fail("""
purs compiler not configured.

Configure your toolchain via Nix:

    [purescript]
    purs = /nix/store/.../bin/purs
    spago = /nix/store/.../bin/spago
    node = /nix/store/.../bin/node

Then run: nix develop
""")
    return path

def _get_spago() -> str:
    """Get spago path from config."""
    path = read_root_config("purescript", "spago", None)
    if path == None:
        fail("spago not configured. See [purescript] section in .buckconfig")
    return path

def _get_node() -> str:
    """Get node path from config."""
    return read_root_config("purescript", "node", "node")

def _get_esbuild() -> str | None:
    """Get esbuild path from config (optional, for modern spago)."""
    return read_root_config("purescript", "esbuild", None)

# ═══════════════════════════════════════════════════════════════════════════════
# PURESCRIPT LIBRARY RULE
# ═══════════════════════════════════════════════════════════════════════════════

def _purescript_library_impl(ctx: AnalysisContext) -> list[Provider]:
    """
    Build a PureScript library using spago.
    
    Compiles PureScript source files using spago which handles dependency
    resolution from the PureScript registry.
    """
    spago = _get_spago()
    
    if not ctx.attrs.srcs:
        return [DefaultInfo(), PureScriptLibraryInfo()]
    
    # Output directory for compiled modules
    output_dir = ctx.actions.declare_output("output", dir = True)
    
    # Build script
    script_parts = ["set -e"]
    
    # Create work directory and copy sources
    script_parts.append("mkdir -p $WORK_DIR/src")
    
    # Copy spago.yaml
    if ctx.attrs.spago_yaml:
        script_parts.append(cmd_args("cp", ctx.attrs.spago_yaml, "$WORK_DIR/spago.yaml", delimiter = " "))
    
    # Copy sources preserving directory structure
    for src in ctx.attrs.srcs:
        script_parts.append(cmd_args("cp --parents", src, "$WORK_DIR/", delimiter = " "))
    
    # Run spago build
    script_parts.append("cd $WORK_DIR")
    script_parts.append(cmd_args(spago, "build", delimiter = " "))
    
    # Copy output
    script_parts.append(cmd_args("cp -r output/* ", output_dir.as_output(), delimiter = ""))
    
    script = cmd_args(script_parts, delimiter = "\n")
    
    cmd = cmd_args(
        "/bin/sh", "-c",
        cmd_args(
            "WORK_DIR=$BUCK_SCRATCH_PATH/work && mkdir -p $WORK_DIR && ",
            script,
            delimiter = "",
        ),
    )
    
    hidden = list(ctx.attrs.srcs)
    if ctx.attrs.spago_yaml:
        hidden.append(ctx.attrs.spago_yaml)
    
    ctx.actions.run(
        cmd_args(cmd, hidden = hidden),
        category = "spago_build",
        identifier = ctx.attrs.name,
        local_only = True,
    )
    
    return [
        DefaultInfo(default_output = output_dir),
        PureScriptLibraryInfo(
            output_dir = output_dir,
            lib_name = ctx.attrs.name,
            deps = ctx.attrs.deps,
        ),
    ]

purescript_library = rule(
    impl = _purescript_library_impl,
    attrs = {
        "srcs": attrs.list(attrs.source(), default = [], doc = "PureScript source files (.purs)"),
        "deps": attrs.list(attrs.dep(), default = [], doc = "PureScript library dependencies"),
        "spago_yaml": attrs.option(attrs.source(), default = None, doc = "spago.yaml configuration file"),
    },
)

# ═══════════════════════════════════════════════════════════════════════════════
# PURESCRIPT APP RULE (for Halogen/web apps)
# ═══════════════════════════════════════════════════════════════════════════════

def _purescript_app_impl(ctx: AnalysisContext) -> list[Provider]:
    """
    Build a PureScript web application.
    
    Uses spago to compile and bundle a Halogen or other web application.
    Produces a dist directory with bundled JS and HTML.
    """
    purs = _get_purs()
    spago = _get_spago()
    node = _get_node()
    
    if not ctx.attrs.srcs:
        fail("purescript_app requires at least one source file")
    
    # Output artifacts - use directory output for dist
    dist_dir = ctx.actions.declare_output("dist", dir = True)
    
    # Build script
    script_parts = ["set -e"]
    
    # Add purs and esbuild to PATH so spago can find them
    esbuild = _get_esbuild()
    if esbuild:
        script_parts.append("export PATH=\"$(dirname {}):$(dirname {}):$PATH\"".format(purs, esbuild))
    else:
        script_parts.append("export PATH=\"$(dirname {}):$PATH\"".format(purs))
    
    # Create work directory structure
    script_parts.append("WORK_DIR=$BUCK_SCRATCH_PATH/work")
    script_parts.append("mkdir -p $WORK_DIR/src/Component")
    
    # Copy spago config (supports both spago.yaml and legacy spago.dhall)
    if ctx.attrs.spago_yaml:
        script_parts.append(cmd_args("cp ", ctx.attrs.spago_yaml, " $WORK_DIR/spago.yaml", delimiter = ""))
        if ctx.attrs.spago_lock:
            script_parts.append(cmd_args("cp ", ctx.attrs.spago_lock, " $WORK_DIR/spago.lock", delimiter = ""))
    elif ctx.attrs.spago_dhall:
        script_parts.append(cmd_args("cp ", ctx.attrs.spago_dhall, " $WORK_DIR/spago.dhall", delimiter = ""))
        if ctx.attrs.packages_dhall:
            script_parts.append(cmd_args("cp ", ctx.attrs.packages_dhall, " $WORK_DIR/packages.dhall", delimiter = ""))
    else:
        fail("purescript_app requires either spago_yaml or spago_dhall")
    
    # Copy source files - put them in src/ relative to WORK_DIR
    for src in ctx.attrs.srcs:
        # Get the relative path after "src/" in the source path
        script_parts.append(cmd_args(
            "mkdir -p \"$WORK_DIR/$(dirname ", src, ")\" && cp ", src, " \"$WORK_DIR/", src, "\"",
            delimiter = "",
        ))
    
    # Run spago build
    script_parts.append("cd $WORK_DIR")
    script_parts.append(cmd_args(spago, " build", delimiter = ""))
    
    # Bundle for browser using spago bundle (uses outfile from spago.yaml)
    script_parts.append(cmd_args(spago, " bundle", delimiter = ""))
    
    # Go back to repo root for copying static files  
    script_parts.append("cd -")  # go back to original dir
    
    # Create dist directory and copy files
    script_parts.append(cmd_args("mkdir -p ", dist_dir.as_output(), delimiter = ""))
    
    # Copy bundle to dist (spago puts it at $WORK_DIR/app.js per spago.yaml)
    script_parts.append(cmd_args("cp $WORK_DIR/app.js ", dist_dir.as_output(), "/app.js", delimiter = ""))
    
    # Copy HTML if provided
    if ctx.attrs.index_html:
        script_parts.append(cmd_args("cp ", ctx.attrs.index_html, " ", dist_dir.as_output(), "/index.html", delimiter = ""))
    
    # Copy CSS if provided
    if ctx.attrs.style_css:
        script_parts.append(cmd_args("cp ", ctx.attrs.style_css, " ", dist_dir.as_output(), "/style.css", delimiter = ""))
    
    script = cmd_args(script_parts, delimiter = "\n")
    
    cmd = cmd_args("/bin/sh", "-c", script)
    
    hidden = list(ctx.attrs.srcs)
    if ctx.attrs.spago_yaml:
        hidden.append(ctx.attrs.spago_yaml)
        if ctx.attrs.spago_lock:
            hidden.append(ctx.attrs.spago_lock)
    if ctx.attrs.spago_dhall:
        hidden.append(ctx.attrs.spago_dhall)
        if ctx.attrs.packages_dhall:
            hidden.append(ctx.attrs.packages_dhall)
    if ctx.attrs.index_html:
        hidden.append(ctx.attrs.index_html)
    if ctx.attrs.style_css:
        hidden.append(ctx.attrs.style_css)
    
    ctx.actions.run(
        cmd_args(cmd, hidden = hidden),
        category = "spago_bundle",
        identifier = ctx.attrs.name,
        local_only = True,
    )
    
    # Create a server script for development
    server_script = ctx.actions.declare_output("{}-serve".format(ctx.attrs.name))
    ctx.actions.write(
        server_script,
        cmd_args(
            "#!/usr/bin/env bash\n",
            "cd \"$(dirname \"$0\")/dist\" && python3 -m http.server ${1:-8080}\n",
            delimiter = "",
        ),
        is_executable = True,
    )
    
    return [
        DefaultInfo(
            default_output = dist_dir,
            sub_targets = {
                "serve": [DefaultInfo(default_outputs = [server_script]), RunInfo(args = cmd_args(server_script))],
            },
        ),
        RunInfo(args = cmd_args(server_script)),
    ]

purescript_app = rule(
    impl = _purescript_app_impl,
    attrs = {
        "srcs": attrs.list(attrs.source(), default = [], doc = "PureScript source files"),
        "spago_yaml": attrs.option(attrs.source(), default = None, doc = "spago.yaml configuration (modern)"),
        "spago_lock": attrs.option(attrs.source(), default = None, doc = "spago.lock file (optional, for reproducibility)"),
        "spago_dhall": attrs.option(attrs.source(), default = None, doc = "spago.dhall configuration (legacy)"),
        "packages_dhall": attrs.option(attrs.source(), default = None, doc = "packages.dhall file (legacy)"),
        "main": attrs.string(default = "Main", doc = "Main module name"),
        "index_html": attrs.option(attrs.source(), default = None, doc = "HTML entry point"),
        "style_css": attrs.option(attrs.source(), default = None, doc = "CSS stylesheet"),
    },
)

# ═══════════════════════════════════════════════════════════════════════════════
# PURESCRIPT BINARY RULE (Node.js executable)
# ═══════════════════════════════════════════════════════════════════════════════

def _purescript_binary_impl(ctx: AnalysisContext) -> list[Provider]:
    """
    Build a PureScript Node.js executable.
    
    Uses spago to compile and bundle for Node.js runtime.
    """
    spago = _get_spago()
    node = _get_node()
    
    if not ctx.attrs.srcs:
        fail("purescript_binary requires at least one source file")
    
    # Output
    bundle_js = ctx.actions.declare_output("{}.js".format(ctx.attrs.name))
    wrapper = ctx.actions.declare_output(ctx.attrs.name)
    
    # Build script
    script_parts = ["set -e"]
    
    # Create work directory
    script_parts.append("WORK_DIR=$BUCK_SCRATCH_PATH/work")
    script_parts.append("mkdir -p $WORK_DIR/src/Component")
    
    # Copy spago.yaml
    script_parts.append(cmd_args("cp", ctx.attrs.spago_yaml, "$WORK_DIR/spago.yaml", delimiter = " "))
    
    # Copy sources
    for src in ctx.attrs.srcs:
        script_parts.append(cmd_args(
            "mkdir -p $WORK_DIR/$(dirname ", src, ") && cp ", src, " $WORK_DIR/", src,
            delimiter = "",
        ))
    
    # Build and bundle
    script_parts.append("cd $WORK_DIR")
    script_parts.append(cmd_args(spago, "build", delimiter = " "))
    script_parts.append(cmd_args(
        spago, "bundle",
        "--module", ctx.attrs.main,
        "--outfile", bundle_js.as_output(),
        "--platform", "node",
        delimiter = " ",
    ))
    
    # Create wrapper
    script_parts.append(cmd_args(
        "cat >", wrapper.as_output(), " << 'EOF'\n",
        "#!/usr/bin/env bash\n",
        "exec ", node, " \"$(dirname \"$0\")/", ctx.attrs.name, ".js\" \"$@\"\n",
        "EOF",
        delimiter = "",
    ))
    script_parts.append(cmd_args("chmod", "+x", wrapper.as_output(), delimiter = " "))
    
    script = cmd_args(script_parts, delimiter = "\n")
    
    cmd = cmd_args("/bin/sh", "-c", script)
    
    hidden = list(ctx.attrs.srcs)
    hidden.append(ctx.attrs.spago_yaml)
    
    ctx.actions.run(
        cmd_args(cmd, hidden = hidden),
        category = "spago_bundle_node",
        identifier = ctx.attrs.name,
        local_only = True,
    )
    
    return [
        DefaultInfo(
            default_output = wrapper,
            sub_targets = {
                "bundle": [DefaultInfo(default_outputs = [bundle_js])],
            },
        ),
        RunInfo(args = cmd_args(wrapper)),
    ]

purescript_binary = rule(
    impl = _purescript_binary_impl,
    attrs = {
        "srcs": attrs.list(attrs.source(), default = [], doc = "PureScript source files"),
        "spago_yaml": attrs.source(doc = "spago.yaml configuration file"),
        "main": attrs.string(default = "Main", doc = "Main module name"),
    },
)

# ═══════════════════════════════════════════════════════════════════════════════
# PURESCRIPT TOOLCHAIN RULE
# ═══════════════════════════════════════════════════════════════════════════════

PureScriptToolchainInfo = provider(fields = {
    "purs": provider_field(str),
    "spago": provider_field(str | None, default = None),
    "node": provider_field(str | None, default = None),
})

def _purescript_toolchain_impl(ctx: AnalysisContext) -> list[Provider]:
    """
    PureScript toolchain with paths from .buckconfig.local.
    """
    purs = read_root_config("purescript", "purs", ctx.attrs.purs)
    spago = read_root_config("purescript", "spago", ctx.attrs.spago)
    node = read_root_config("purescript", "node", ctx.attrs.node)

    return [
        DefaultInfo(),
        PureScriptToolchainInfo(
            purs = purs,
            spago = spago,
            node = node,
        ),
    ]

purescript_toolchain = rule(
    impl = _purescript_toolchain_impl,
    attrs = {
        "purs": attrs.string(default = "purs", doc = "Path to purs compiler"),
        "spago": attrs.option(attrs.string(), default = None, doc = "Path to spago"),
        "node": attrs.option(attrs.string(), default = None, doc = "Path to node"),
    },
    is_toolchain_rule = True,
)

def _system_purescript_toolchain_impl(_ctx: AnalysisContext) -> list[Provider]:
    """System PureScript toolchain - DISABLED."""
    fail("""
system_purescript_toolchain is disabled.

Configure your PureScript toolchain via Nix:

    [purescript]
    purs = /nix/store/.../bin/purs
    spago = /nix/store/.../bin/spago
    node = /nix/store/.../bin/node

Then run: nix develop
""")

system_purescript_toolchain = rule(
    impl = _system_purescript_toolchain_impl,
    attrs = {},
    is_toolchain_rule = True,
)
