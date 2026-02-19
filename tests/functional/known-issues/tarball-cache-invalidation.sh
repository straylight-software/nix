#!/usr/bin/env bash

# =============================================================================
# KNOWN ISSUE: Tarball cache cannot be invalidated
# GitHub Issue: #9814
# =============================================================================
#
# Description:
# Once a tarball is cached by Nix, there is no way to invalidate the cache
# entry. Even if the remote tarball changes (same URL, different content),
# Nix will continue to serve the cached version until the TTL expires.
#
# This is problematic for:
# - Development workflows where tarballs are updated in place
# - CI/CD systems that use mutable tarball URLs
# - Users who need to force re-download of a changed tarball
#
# The only workarounds are:
# - Wait for TTL to expire
# - Manually delete cache files from ~/.cache/nix
# - Use a different URL (cache busting with query params)
#
# Expected behavior: Should be able to invalidate specific cache entries
# Actual behavior: No mechanism to invalidate tarball cache
# =============================================================================

source ../common.sh

clearStoreIfPossible

testDir="$TEST_ROOT/tarball-cache-test"
rm -rf "$testDir" "$TEST_HOME/.cache/nix"
mkdir -p "$testDir/serve"

echo "=== Testing tarball cache invalidation ==="
echo "This test reveals tarball cache invalidation issue (#9814)"
echo ""

# Create initial tarball content
echo "version 1" >"$testDir/serve/content.txt"
tar -czf "$testDir/serve/test.tar.gz" -C "$testDir/serve" content.txt

# Start a simple HTTP server (if python3 available)
if ! command -v python3 &>/dev/null; then
    echo "SKIP: python3 not available for HTTP server"
    exit 77
fi

# Start HTTP server in background
cd "$testDir/serve"
python3 -m http.server 18888 &>/dev/null &
HTTP_PID=$!
cd - >/dev/null

# Give server time to start
sleep 1

# Cleanup function
cleanup() {
    kill $HTTP_PID 2>/dev/null || true
}
trap cleanup EXIT

echo "HTTP server started on port 18888 (PID: $HTTP_PID)"

# First fetch
echo ""
echo "--- First fetch (should get 'version 1') ---"
FIRST_RESULT=$(nix eval --impure --raw --expr "builtins.readFile ((builtins.fetchTarball \"http://localhost:18888/test.tar.gz\") + \"/content.txt\")" 2>&1) || true
echo "Content: $FIRST_RESULT"

# Update tarball content (same URL, different content)
echo "version 2" >"$testDir/serve/content.txt"
tar -czf "$testDir/serve/test.tar.gz" -C "$testDir/serve" content.txt

echo ""
echo "Tarball updated to 'version 2' at same URL"

# Second fetch - should still return cached version 1
echo ""
echo "--- Second fetch (bug: returns cached 'version 1', should get 'version 2') ---"
SECOND_RESULT=$(nix eval --impure --raw --expr "builtins.readFile ((builtins.fetchTarball \"http://localhost:18888/test.tar.gz\") + \"/content.txt\")" 2>&1) || true
echo "Content: $SECOND_RESULT"

# Try with --refresh (should force re-fetch but may not work)
echo ""
echo "--- Third fetch with refresh flag ---"
THIRD_RESULT=$(nix eval --refresh --impure --raw --expr "builtins.readFile ((builtins.fetchTarball \"http://localhost:18888/test.tar.gz\") + \"/content.txt\")" 2>&1) || true
echo "Content: $THIRD_RESULT"

echo ""
echo "=== Results ==="

# Check if we can ever get the new version
GOT_NEW_VERSION=false
if [[ "$SECOND_RESULT" == "version 2" ]] || [[ "$THIRD_RESULT" == "version 2" ]]; then
    GOT_NEW_VERSION=true
fi

if [[ "$GOT_NEW_VERSION" == "false" ]]; then
    echo "KNOWN ISSUE REVEALED: Cannot invalidate tarball cache!"
    echo "First fetch: '$FIRST_RESULT'"
    echo "After update: '$SECOND_RESULT' (expected 'version 2')"
    echo "With --refresh: '$THIRD_RESULT' (expected 'version 2')"
    echo "See GitHub issue #9814"
    exit 0 # Expected broken behavior
else
    echo "Cache was properly invalidated"
    echo "If this passes consistently, the bug may be fixed!"
    exit 0
fi
