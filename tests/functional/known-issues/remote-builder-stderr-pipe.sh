#!/usr/bin/env bash

# =============================================================================
# KNOWN ISSUE: Remote builder stderr pipe filling causes hangs
# GitHub Issue: #5701
# =============================================================================
#
# Description:
# When using remote builders, if a build produces a large amount of stderr
# output, the stderr pipe can fill up and block the build process. This
# causes the build to hang indefinitely.
#
# The issue occurs because:
# 1. The build hook creates pipes for stdout/stderr communication
# 2. If the remote build produces stderr faster than it's consumed
# 3. The pipe buffer fills up (typically 64KB on Linux)
# 4. The remote build blocks trying to write to stderr
# 5. The local nix process may be waiting for something else
# 6. Deadlock occurs
#
# This is a fundamental issue with the pipe/fd handling in the build hook
# mechanism. The binary-cache-build-remote.sh test is skipped with comment:
# "remote builders disabled - build hook is unsound"
#
# Expected behavior: Builds with large stderr should complete
# Actual behavior: Builds hang when stderr pipe fills
# =============================================================================

source ../common.sh

echo "=== Remote Builder stderr Pipe Filling Issue ==="
echo ""
echo "This test documents a known issue with remote builder pipe handling."
echo ""
echo "The issue: When remote builds produce large stderr output, the stderr"
echo "pipe can fill up and cause deadlocks."
echo ""
echo "Evidence:"
echo "  1. tests/functional/binary-cache-build-remote.sh is SKIPPED"
echo "     with comment: 'remote builders disabled - build hook is unsound'"
echo ""
echo "  2. GitHub issue #5701 documents this pipe buffer deadlock"
echo ""
echo "Root cause:"
echo "  - Pipe buffers are limited (typically 64KB on Linux)"
echo "  - If stderr output exceeds buffer before being consumed, writes block"
echo "  - If the consumer is waiting for something else, deadlock occurs"
echo ""

# Demonstrate the pipe buffer limit locally
echo "--- Demonstrating pipe buffer behavior ---"
echo ""

# Create a derivation that produces lots of stderr
TEST_DIR="$TEST_ROOT/stderr-pipe-test"
rm -rf "$TEST_DIR"
mkdir -p "$TEST_DIR"

# This is a simplified local test - the real issue manifests with remote builders
cat >"$TEST_DIR/verbose-builder.sh" <<'BUILDER'
#!/bin/sh
# Produce a lot of stderr output
for i in $(seq 1 10000); do
  echo "stderr line $i: some verbose build output that fills the pipe" >&2
done
echo "done" > $out
BUILDER
chmod +x "$TEST_DIR/verbose-builder.sh"

echo "Created a builder that produces ~10000 lines of stderr"
echo ""
echo "In a remote builder scenario with unsound pipe handling:"
echo "  - This output would fill the stderr pipe buffer"
echo "  - If not consumed promptly, the builder would block"
echo "  - If the local process is waiting for something else, deadlock"
echo ""

# Try to build locally (should work, issue is with remote)
echo "--- Local build (should succeed) ---"

clearStoreIfPossible

RESULT=$(nix-build --no-out-link -E "
  derivation {
    name = \"verbose-stderr-test\";
    system = builtins.currentSystem;
    builder = \"/bin/sh\";
    args = [\"-c\" ''
      for i in \$(seq 1 1000); do
        echo \"stderr line \$i\" >&2
      done
      echo done > \$out
    ''];
  }
" 2>&1) || true

if [[ -n $RESULT ]] && [[ $RESULT == /nix/store/* ]]; then
  echo "Local build succeeded (expected - issue is with remote builders)"
else
  echo "Local build result: $RESULT"
  echo "(May fail in sandboxed environments without /bin/sh)"
fi

echo ""
echo "=== To reproduce the actual issue ==="
echo "1. Set up a remote builder via SSH"
echo "2. Create a derivation that produces >64KB of stderr"
echo "3. Build it via the remote builder"
echo "4. Observe the build hanging when stderr buffer fills"
echo ""
echo "Workaround: Redirect builder stderr to a file instead of pipe"
echo ""
echo "KNOWN ISSUE: Remote builder pipe/fd handling is unsound (#5701)"
exit 0
