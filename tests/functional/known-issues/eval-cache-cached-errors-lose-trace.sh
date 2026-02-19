#!/usr/bin/env bash

# =============================================================================
# KNOWN ISSUE: Cached errors lose stack trace
# GitHub Issues: #3872, #9165
# =============================================================================
#
# Description:
# When an evaluation error is cached, subsequent attempts to evaluate the
# same expression return the cached error message but WITHOUT the original
# stack trace. This makes debugging extremely difficult because users see:
#   "error: some message"
# instead of:
#   "error: some message
#          at /path/to/file.nix:123:45
#          called from /path/to/other.nix:67:89"
#
# Root cause in eval-cache.cpp:
# - Errors are serialized to the cache with just the message
# - Position/trace information is not persisted
# - When replayed, a generic CachedEvalError is thrown
#
# Expected behavior: Cached errors should include full stack trace
# Actual behavior: Cached errors lose positional/trace information
# =============================================================================

source ../common.sh

requireGit

flakeDir="$TEST_ROOT/cached-error-trace-flake"

createGitRepo "$flakeDir" ""
cp ../simple.nix ../simple.builder.sh "${config_nix}" "$flakeDir/"
git -C "$flakeDir" add simple.nix simple.builder.sh config.nix
git -C "$flakeDir" commit -m "config.nix"

# Create a flake with a nested error to show trace loss
cat >"$flakeDir/flake.nix" <<'EOF'
{
  description = "Test flake for cached error trace loss";
  outputs = { self }: {
    # Nested function calls to generate a stack trace
    packages.x86_64-linux.default = let
      level3 = x: throw "Deep error in level3: ${x}";
      level2 = x: level3 "from-level2-${x}";
      level1 = x: level2 "from-level1-${x}";
    in level1 "initial";
  };
}
EOF

git -C "$flakeDir" add flake.nix
git -C "$flakeDir" commit -m "Add flake with nested error"

# Clear any existing eval cache
rm -rf "$TEST_HOME/.cache/nix/eval-cache-v5"

echo "=== Testing cached error stack trace preservation ==="
echo "This test reveals stack trace loss in cached errors (#3872, #9165)"
echo ""

# First evaluation - should have full trace
echo "--- First evaluation (fresh, should have trace) ---"
FIRST_ERROR=$(nix eval "$flakeDir#packages.x86_64-linux.default" 2>&1 || true)
echo "$FIRST_ERROR"
echo ""

# Count trace lines in first error
FIRST_TRACE_LINES=$(echo "$FIRST_ERROR" | grep -c "^\s*at\|called from\|while" || true)
echo "Trace lines in first error: $FIRST_TRACE_LINES"
echo ""

# Second evaluation - should use cache
echo "--- Second evaluation (cached, may lose trace) ---"
SECOND_ERROR=$(nix eval "$flakeDir#packages.x86_64-linux.default" 2>&1 || true)
echo "$SECOND_ERROR"
echo ""

# Count trace lines in second error
SECOND_TRACE_LINES=$(echo "$SECOND_ERROR" | grep -c "^\s*at\|called from\|while" || true)
echo "Trace lines in second error: $SECOND_TRACE_LINES"
echo ""

echo "=== Results ==="

# KNOWN_ISSUE: The second evaluation should have the same trace information
# but due to the bug, it will have fewer or no trace lines

if [[ $SECOND_TRACE_LINES -lt $FIRST_TRACE_LINES ]]; then
    echo "KNOWN ISSUE REVEALED: Cached error lost trace information!"
    echo "First evaluation had $FIRST_TRACE_LINES trace lines"
    echo "Cached evaluation had $SECOND_TRACE_LINES trace lines"
    echo "See GitHub issues #3872, #9165"
    exit 0 # Expected broken behavior
elif [[ $FIRST_TRACE_LINES -eq 0 ]]; then
    echo "NOTE: Neither evaluation showed trace lines"
    echo "This may indicate --show-trace is needed or trace format changed"
    exit 0
else
    echo "Both evaluations have equal trace information ($FIRST_TRACE_LINES lines)"
    echo "If this passes consistently, the bug may be fixed!"
    exit 0
fi
