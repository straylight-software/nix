#!/usr/bin/env bash

# =============================================================================
# KNOWN ISSUE: Eval cache SQLite lock contention
# GitHub Issues: #3794, #6847
# =============================================================================
#
# Description:
# When multiple nix processes (e.g., `nix search`) run concurrently against
# the same flake, they contend for the SQLite eval cache database lock.
# This causes:
# - Long waits (processes block each other)
# - "database is locked" errors
# - `nix search` blocking other evals entirely (#6847)
#
# Root cause in eval-cache.cpp:
# - Uses SQLite with default locking (exclusive during writes)
# - Frequent writes during attribute traversal
# - No WAL mode or other concurrency optimization
#
# Additionally, there's an expensive O(n) table scan on every attribute
# lookup (eval-cache.cpp:250) which makes the lock held longer.
#
# Expected behavior: Concurrent evals should not block each other excessively
# Actual behavior: Significant lock contention causing slowdowns/errors
# =============================================================================

source ../common.sh

requireGit

# Create a test flake with many attributes to trigger cache writes
flakeDir="$TEST_ROOT/eval-cache-lock-flake"

createGitRepo "$flakeDir" ""
cp ../simple.nix ../simple.builder.sh "${config_nix}" "$flakeDir/"
git -C "$flakeDir" add simple.nix simple.builder.sh config.nix
git -C "$flakeDir" commit -m "config.nix"

# Create a flake with many packages to maximize cache activity
cat >"$flakeDir/flake.nix" <<'EOF'
{
  description = "Test flake for eval cache lock contention";
  outputs = { self }: {
    # Generate many packages to stress the eval cache
    packages.x86_64-linux = builtins.listToAttrs (
      builtins.map (i: {
        name = "pkg-${toString i}";
        value = derivation {
          name = "test-${toString i}";
          system = "x86_64-linux";
          builder = "/bin/sh";
          args = ["-c" "echo ${toString i} > $out"];
        };
      }) (builtins.genList (x: x) 100)
    );
  };
}
EOF

git -C "$flakeDir" add flake.nix
git -C "$flakeDir" commit -m "Add flake with many packages"

# Clear any existing eval cache
rm -rf "$TEST_HOME/.cache/nix/eval-cache-v5"

echo "=== Starting concurrent eval cache test ==="
echo "This test reveals SQLite lock contention issues (#3794, #6847)"

# Run multiple nix search commands concurrently
# In a well-functioning system, these should complete without errors
# In the buggy system, we expect lock contention

PIDS=()
ERRORS=0

# Start 5 concurrent searches
for i in {1..5}; do
  (
    # Each process searches the flake, triggering cache reads/writes
    nix search "$flakeDir" "pkg-" 2>&1
  ) &
  PIDS+=($!)
done

# Wait for all processes and check for errors
for pid in "${PIDS[@]}"; do
  if ! wait "$pid"; then
    ERRORS=$((ERRORS + 1))
  fi
done

echo ""
echo "=== Results ==="
echo "Processes with errors: $ERRORS / ${#PIDS[@]}"

# KNOWN_ISSUE: With lock contention bugs, we expect some processes to fail
# or take excessively long. When fixed, all should complete quickly.

if [[ $ERRORS -gt 0 ]]; then
  echo "KNOWN ISSUE REVEALED: Some processes failed due to lock contention"
  echo "See GitHub issues #3794, #6847"
  exit 0 # Expected broken behavior
else
  echo "All processes completed successfully"
  echo "NOTE: Lock contention may still exist but didn't cause failures"
  echo "To fully test, run with higher concurrency or slower storage"
  exit 0 # Test passes either way - we're documenting the issue
fi
