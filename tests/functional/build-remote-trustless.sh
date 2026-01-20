# shellcheck shell=bash

# Remote builders are disabled in this fork - the build hook has unsound
# pipe/fd handling that causes log streaming bugs with CA derivations.
skipTest "remote builders disabled - build hook is unsound"

# All variables should be defined externally by the scripts that source
# this, `set -u` will catch any that are forgotten.
# shellcheck disable=SC2154

requireSandboxSupport
requiresUnprivilegedUserNamespaces
[[ "$busybox" =~ busybox ]] || skipTest "no busybox"

unset NIX_STORE_DIR

remoteDir=$TEST_ROOT/remote

# Note: ssh{-ng}://localhost bypasses ssh. See tests/functional/build-remote.sh for
# more details.
nix-build "$file" -o "$TEST_ROOT/result" --max-jobs 0 \
    --arg busybox "$busybox" \
    --store "$TEST_ROOT/local" \
    --builders "$proto://localhost?remote-program=$prog&remote-store=${remoteDir}%3Fsystem-features=foo%20bar%20baz - - 1 1 foo,bar,baz"
