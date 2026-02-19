#!/usr/bin/env bash

# https://github.com/NixOS/nix/issues/13247

export NIX_TESTS_CA_BY_DEFAULT=1

source common.sh

clearStoreIfPossible

set -x

# Build derivation (both outputs)
nix build -f issue-13247.nix --json a a-prime use-a-more-outputs --no-link >"$TEST_ROOT"/a.json

cache="file://$TEST_ROOT/cache"

# Copy all outputs and realisations to cache
declare -a drvs
for d in "$NIX_STORE_DIR"/*-issue-13247-a.drv "$NIX_STORE_DIR"/*-use-a-more-outputs.drv; do
  drvs+=("$d" "$d"^*)
done
nix copy --to "$cache" "${drvs[@]}"

function delete() {
  # Delete local copy
  # shellcheck disable=SC2046
  nix-store --delete \
    $(jq -r '.[] | .drvPath, .outputs.[]' <"$TEST_ROOT"/a.json) \
    "$NIX_STORE_DIR"/*-issue-13247-a.drv \
    "$NIX_STORE_DIR"/*-use-a-more-outputs.drv

  [[ ! -e "$(jq -r '.[0].outputs.out' <"$TEST_ROOT"/a.json)" ]]
  [[ ! -e "$(jq -r '.[1].outputs.out' <"$TEST_ROOT"/a.json)" ]]
  [[ ! -e "$(jq -r '.[2].outputs.first' <"$TEST_ROOT"/a.json)" ]]
  [[ ! -e "$(jq -r '.[2].outputs.second' <"$TEST_ROOT"/a.json)" ]]
}

delete

buildViaSubstitute() {
  nix build -f issue-13247.nix "$1" --no-link --max-jobs 0 --substituters "$cache" --no-require-sigs --offline --substitute
}

# Substitue just the first output
buildViaSubstitute use-a-more-outputs^first

# Should only fetch the output we asked for
[[ -d "$(jq -r '.[0].outputs.out' <"$TEST_ROOT"/a.json)" ]]
[[ -f "$(jq -r '.[2].outputs.first' <"$TEST_ROOT"/a.json)" ]]
[[ ! -e "$(jq -r '.[2].outputs.second' <"$TEST_ROOT"/a.json)" ]]

delete

# Failure with 2.28 encountered in CI
requireDaemonNewerThan "2.29"

# Substitue just the first output
#
# This derivation is the same after normalization, so we should get
# early cut-off, and thus a chance to download just the output we want
# rather than building more
buildViaSubstitute use-a-prime-more-outputs^first

# Should only fetch the output we asked for
[[ -d "$(jq -r '.[0].outputs.out' <"$TEST_ROOT"/a.json)" ]]
[[ -f "$(jq -r '.[2].outputs.first' <"$TEST_ROOT"/a.json)" ]]
[[ ! -e "$(jq -r '.[2].outputs.second' <"$TEST_ROOT"/a.json)" ]]
