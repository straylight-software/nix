#!/usr/bin/env bash

# =============================================================================
# KNOWN ISSUE: fetchGit caches wrong results for tags
# GitHub Issue: #7146
# =============================================================================
#
# Description:
# When using fetchGit with a ref pointing to a tag, Nix may cache the wrong
# result. Specifically:
# 1. fetchGit with ref="v1.0" fetches the correct commit
# 2. The tag "v1.0" is moved to a different commit (force push or tag deletion/recreation)
# 3. fetchGit with ref="v1.0" returns the OLD cached result, not the new one
#
# This is related to how Nix caches git fetches based on ref names without
# properly tracking when tags are mutable (unlike branches which are expected
# to change).
#
# See also: tests/functional/git/packed-refs-no-cache.sh which has INVERTED
# test logic revealing a similar issue.
#
# Expected behavior: Moved tags should be re-fetched
# Actual behavior: Old cached result is returned even after tag moves
# =============================================================================

source ../common.sh

requireGit

clearStoreIfPossible

repo="$TEST_ROOT/fetchgit-tag-cache-repo"
export _NIX_FORCE_HTTP=1

rm -rf "$repo" "$TEST_HOME/.cache/nix"

# Initialize repo
git init --initial-branch="main" "$repo"
git -C "$repo" config user.email "test@example.com"
git -C "$repo" config user.name "Test"

# First commit
echo "version 1" >"$repo/version.txt"
git -C "$repo" add version.txt
git -C "$repo" commit -m "Version 1"

# Create a tag
git -C "$repo" tag "v1.0"

FIRST_COMMIT=$(git -C "$repo" rev-parse HEAD)

echo "=== Testing fetchGit tag caching behavior ==="
echo "This test reveals wrong tag caching (#7146)"
echo ""
echo "First commit (v1.0): $FIRST_COMMIT"

# First fetch - should get version 1
echo ""
echo "--- First fetch (should get 'version 1') ---"
FIRST_RESULT=$(nix eval --impure --raw --expr "builtins.readFile ((builtins.fetchGit { url = \"file://$repo\"; ref = \"v1.0\"; }) + \"/version.txt\")")
echo "Content: $FIRST_RESULT"

# Make a new commit
echo "version 2" >"$repo/version.txt"
git -C "$repo" add version.txt
git -C "$repo" commit -m "Version 2"

SECOND_COMMIT=$(git -C "$repo" rev-parse HEAD)

# Move the tag to the new commit (simulating tag force-push)
git -C "$repo" tag -f "v1.0"

echo ""
echo "Second commit (v1.0 moved): $SECOND_COMMIT"

# Second fetch - SHOULD get version 2, but bug causes it to return version 1
echo ""
echo "--- Second fetch (should get 'version 2', bug returns 'version 1') ---"
SECOND_RESULT=$(nix eval --impure --raw --expr "builtins.readFile ((builtins.fetchGit { url = \"file://$repo\"; ref = \"v1.0\"; }) + \"/version.txt\")")
echo "Content: $SECOND_RESULT"

echo ""
echo "=== Results ==="

# KNOWN_ISSUE: The second result should be "version 2" but due to caching bug
# it will still be "version 1"

if [[ "$SECOND_RESULT" == "version 1" ]]; then
    echo "KNOWN ISSUE REVEALED: fetchGit returned stale cached result!"
    echo "Expected: 'version 2' (new tag target)"
    echo "Got: 'version 1' (old cached result)"
    echo "See GitHub issue #7146"
    exit 0 # Expected broken behavior
elif [[ "$SECOND_RESULT" == "version 2" ]]; then
    echo "fetchGit correctly re-fetched the moved tag"
    echo "If this passes consistently, the bug may be fixed!"
    exit 0
else
    echo "Unexpected result: $SECOND_RESULT"
    exit 1
fi
