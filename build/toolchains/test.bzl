# // straylight // nix // toolchains // test
#
# Test toolchain utilities

load("@prelude//tests:remote_test_execution_toolchain.bzl", "RemoteTestExecutionToolchainInfo")

def _noop_remote_test_execution_toolchain_impl(_ctx: AnalysisContext) -> list[Provider]:
    """
    A no-op remote test execution toolchain for local-only test execution.
    """
    return [
        DefaultInfo(),
        RemoteTestExecutionToolchainInfo(
            default_profile = None,
            profiles = {},
            default_run_as_bundle = False,
        ),
    ]

noop_remote_test_execution_toolchain = rule(
    impl = _noop_remote_test_execution_toolchain_impl,
    attrs = {},
    is_toolchain_rule = True,
)
