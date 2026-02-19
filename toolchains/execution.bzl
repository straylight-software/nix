# toolchains/execution.bzl
#
# Execution platforms for Buck2 remote execution (LRE).
#
# By default, the prelude's execution_platform has remote_enabled=False.
# These platforms enable remote execution for NativeLink.

def _lre_execution_platform_impl(ctx: AnalysisContext) -> list[Provider]:
    """Execution platform with remote execution enabled."""
    constraints = dict()
    constraints.update(ctx.attrs.cpu_configuration[ConfigurationInfo].constraints)
    constraints.update(ctx.attrs.os_configuration[ConfigurationInfo].constraints)
    cfg = ConfigurationInfo(constraints = constraints, values = {})

    name = ctx.label.raw_target()

    # Build executor config based on whether remote is enabled
    if ctx.attrs.remote_enabled:
        executor_config = CommandExecutorConfig(
            local_enabled = ctx.attrs.local_enabled,
            remote_enabled = True,
            use_windows_path_separators = False,
            # RE properties - platform capabilities for worker matching
            # nix-worker matches both local NixOS workers and Fly.io workers
            remote_execution_properties = {
                "OSFamily": "linux",
                "container-image": "nix-worker",
            },
            remote_execution_use_case = "buck2-default",
            remote_output_paths = "output_paths",
        )
    else:
        executor_config = CommandExecutorConfig(
            local_enabled = ctx.attrs.local_enabled,
            remote_enabled = False,
            use_windows_path_separators = False,
        )

    platform = ExecutionPlatformInfo(
        label = name,
        configuration = cfg,
        executor_config = executor_config,
    )

    return [
        DefaultInfo(),
        platform,
        PlatformInfo(label = str(name), configuration = cfg),
        ExecutionPlatformRegistrationInfo(platforms = [platform]),
    ]

lre_execution_platform = rule(
    impl = _lre_execution_platform_impl,
    attrs = {
        "cpu_configuration": attrs.dep(providers = [ConfigurationInfo]),
        "os_configuration": attrs.dep(providers = [ConfigurationInfo]),
        "local_enabled": attrs.bool(default = True),
        "remote_enabled": attrs.bool(default = True),
    },
)

def _host_cpu_configuration() -> str:
    arch = host_info().arch
    if arch.is_aarch64:
        return "prelude//cpu:arm64"
    elif arch.is_arm:
        return "prelude//cpu:arm32"
    elif arch.is_i386:
        return "prelude//cpu:x86_32"
    else:
        return "prelude//cpu:x86_64"

def _host_os_configuration() -> str:
    os = host_info().os
    if os.is_macos:
        return "prelude//os:macos"
    elif os.is_windows:
        return "prelude//os:windows"
    else:
        return "prelude//os:linux"

host_configuration = struct(
    cpu = _host_cpu_configuration(),
    os = _host_os_configuration(),
)
