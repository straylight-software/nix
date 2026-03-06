#!/usr/bin/env bash
# stress-test-firecracker.sh - Extreme stress tests for Firecracker build service
#
# Usage:
#   ./scripts/stress-test-firecracker.sh [test-name]
#
# Tests:
#   all         - Run all tests (default)
#   parallel    - Parallel builds (10 concurrent)
#   sequential  - Many sequential builds (50)
#   large-input - Build with large input closure
#   large-output - Build generating large output
#   long-running - Long-running build (5 minutes)
#   failure     - Build failure recovery
#   chain       - Dependency chain (10 levels)

set -euo pipefail

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Configuration
PARALLEL_COUNT=${PARALLEL_COUNT:-10}
SEQUENTIAL_COUNT=${SEQUENTIAL_COUNT:-50}
CHAIN_DEPTH=${CHAIN_DEPTH:-10}
LONG_RUNNING_SECONDS=${LONG_RUNNING_SECONDS:-300}
LARGE_OUTPUT_MB=${LARGE_OUTPUT_MB:-500}

# Results tracking
declare -A TEST_RESULTS
TOTAL_PASSED=0
TOTAL_FAILED=0

log_info() { echo -e "${BLUE}[INFO]${NC} $*"; }
log_pass() { echo -e "${GREEN}[PASS]${NC} $*"; }
log_fail() { echo -e "${RED}[FAIL]${NC} $*"; }
log_warn() { echo -e "${YELLOW}[WARN]${NC} $*"; }

# Get bash path from nix
get_bash_path() {
  nix eval --raw nixpkgs#bash.outPath 2>/dev/null || echo "/nix/store/$(ls /nix/store | grep -E '^[a-z0-9]+-bash-[0-9]' | head -1)"
}

BASH_PATH=$(get_bash_path)
log_info "Using bash: $BASH_PATH"

# Helper: run a build and measure time
run_build() {
  local name="$1"
  local expr="$2"
  local start end duration

  start=$(date +%s.%N)
  if nix build --expr "$expr" --builders '' --no-link 2>&1; then
    end=$(date +%s.%N)
    duration=$(echo "$end - $start" | bc)
    echo "$duration"
    return 0
  else
    return 1
  fi
}

# Test 1: Parallel builds
test_parallel() {
  log_info "=== Test: Parallel Builds ($PARALLEL_COUNT concurrent) ==="

  local pids=()
  local results_dir
  results_dir=$(mktemp -d)

  for i in $(seq 1 "$PARALLEL_COUNT"); do
    (
      local expr="derivation { 
        name = \"parallel-$i\"; 
        builder = \"$BASH_PATH/bin/bash\"; 
        args = [\"-c\" \"echo 'Build $i' && sleep 2 && echo done > \\\$out\"];
        system = \"x86_64-linux\";
      }"
      if run_build "parallel-$i" "$expr" >"$results_dir/$i.time" 2>&1; then
        echo "pass" >"$results_dir/$i.status"
      else
        echo "fail" >"$results_dir/$i.status"
      fi
    ) &
    pids+=($!)
  done

  # Wait for all
  local failed=0
  for pid in "${pids[@]}"; do
    if ! wait "$pid"; then
      ((failed++)) || true
    fi
  done

  # Count results
  local passed
  passed=$(grep -l "pass" "$results_dir"/*.status 2>/dev/null | wc -l)
  failed=$((PARALLEL_COUNT - passed))

  rm -rf "$results_dir"

  if [[ $failed -eq 0 ]]; then
    log_pass "All $PARALLEL_COUNT parallel builds succeeded"
    return 0
  else
    log_fail "$failed/$PARALLEL_COUNT parallel builds failed"
    return 1
  fi
}

# Test 2: Sequential builds
test_sequential() {
  log_info "=== Test: Sequential Builds ($SEQUENTIAL_COUNT builds) ==="

  local passed=0
  local failed=0
  local total_time=0

  for i in $(seq 1 "$SEQUENTIAL_COUNT"); do
    local expr="derivation { 
      name = \"sequential-$i\"; 
      builder = \"$BASH_PATH/bin/bash\"; 
      args = [\"-c\" \"echo $i > \\\$out\"];
      system = \"x86_64-linux\";
    }"

    local duration
    if duration=$(run_build "sequential-$i" "$expr"); then
      ((passed++))
      total_time=$(echo "$total_time + $duration" | bc)
      # Progress every 10
      if ((i % 10 == 0)); then
        log_info "Progress: $i/$SEQUENTIAL_COUNT"
      fi
    else
      ((failed++))
      log_warn "Build $i failed"
    fi
  done

  local avg_time
  avg_time=$(echo "scale=2; $total_time / $passed" | bc)

  if [[ $failed -eq 0 ]]; then
    log_pass "All $SEQUENTIAL_COUNT sequential builds succeeded (avg: ${avg_time}s)"
    return 0
  else
    log_fail "$failed/$SEQUENTIAL_COUNT sequential builds failed"
    return 1
  fi
}

# Test 3: Large input closure
test_large_input() {
  log_info "=== Test: Large Input Closure ==="

  # Use a package with large closure (gcc has ~400 deps)
  local expr='
    let pkgs = import <nixpkgs> {}; in
    derivation {
      name = "large-input-test";
      builder = "${pkgs.bash}/bin/bash";
      args = ["-c" "echo \"Closure size: $(du -sh /nix/store | head -1)\" && echo done > $out"];
      system = "x86_64-linux";
      # Reference gcc to pull in its closure
      gcc = pkgs.gcc;
      PATH = "${pkgs.coreutils}/bin";
    }
  '

  if run_build "large-input" "$expr" >/dev/null 2>&1; then
    log_pass "Large input closure build succeeded"
    return 0
  else
    log_fail "Large input closure build failed"
    return 1
  fi
}

# Test 4: Large output
test_large_output() {
  log_info "=== Test: Large Output ($LARGE_OUTPUT_MB MB) ==="

  local expr="derivation { 
    name = \"large-output\"; 
    builder = \"$BASH_PATH/bin/bash\"; 
    args = [\"-c\" \"dd if=/dev/zero of=\\\$out bs=1M count=$LARGE_OUTPUT_MB 2>/dev/null\"];
    system = \"x86_64-linux\";
  }"

  local duration
  if duration=$(run_build "large-output" "$expr"); then
    local rate
    rate=$(echo "scale=2; $LARGE_OUTPUT_MB / $duration" | bc)
    log_pass "Large output ($LARGE_OUTPUT_MB MB) in ${duration}s (${rate} MB/s)"
    return 0
  else
    log_fail "Large output build failed"
    return 1
  fi
}

# Test 5: Long-running build
test_long_running() {
  log_info "=== Test: Long-Running Build ($LONG_RUNNING_SECONDS seconds) ==="

  local expr="derivation { 
    name = \"long-running\"; 
    builder = \"$BASH_PATH/bin/bash\"; 
    args = [\"-c\" \"
      for i in \\\$(seq 1 $LONG_RUNNING_SECONDS); do
        echo \\\"Heartbeat \\\$i/$LONG_RUNNING_SECONDS\\\";
        sleep 1;
      done;
      echo done > \\\$out
    \"];
    system = \"x86_64-linux\";
  }"

  local duration
  if duration=$(run_build "long-running" "$expr"); then
    log_pass "Long-running build completed in ${duration}s"
    return 0
  else
    log_fail "Long-running build failed or timed out"
    return 1
  fi
}

# Test 6: Build failure recovery
test_failure() {
  log_info "=== Test: Build Failure Recovery ==="

  # First, a build that fails
  local fail_expr="derivation { 
    name = \"intentional-failure\"; 
    builder = \"$BASH_PATH/bin/bash\"; 
    args = [\"-c\" \"echo 'About to fail' && exit 1\"];
    system = \"x86_64-linux\";
  }"

  if nix build --expr "$fail_expr" --builders '' --no-link 2>&1; then
    log_fail "Build should have failed but succeeded"
    return 1
  fi

  log_info "First build failed as expected, testing recovery..."

  # Now a build that should succeed
  local success_expr="derivation { 
    name = \"after-failure\"; 
    builder = \"$BASH_PATH/bin/bash\"; 
    args = [\"-c\" \"echo 'Recovery successful' > \\\$out\"];
    system = \"x86_64-linux\";
  }"

  if run_build "after-failure" "$success_expr" >/dev/null 2>&1; then
    log_pass "Build recovered successfully after failure"
    return 0
  else
    log_fail "Build failed to recover after previous failure"
    return 1
  fi
}

# Test 7: Dependency chain
test_chain() {
  log_info "=== Test: Dependency Chain ($CHAIN_DEPTH levels) ==="

  # Build a chain: each derivation depends on the previous
  local prev_path=""
  local failed=0

  for i in $(seq 1 "$CHAIN_DEPTH"); do
    local deps=""
    if [[ -n "$prev_path" ]]; then
      deps="prev = builtins.storePath \"$prev_path\";"
    fi

    local expr="derivation { 
      name = \"chain-$i\"; 
      builder = \"$BASH_PATH/bin/bash\"; 
      args = [\"-c\" \"echo 'Level $i' > \\\$out\"];
      system = \"x86_64-linux\";
      $deps
    }"

    local output
    if output=$(nix build --expr "$expr" --builders '' --print-out-paths 2>&1); then
      prev_path=$(echo "$output" | tail -1)
      log_info "Chain level $i: $prev_path"
    else
      log_fail "Chain failed at level $i"
      ((failed++))
      break
    fi
  done

  if [[ $failed -eq 0 ]]; then
    log_pass "Dependency chain ($CHAIN_DEPTH levels) completed"
    return 0
  else
    return 1
  fi
}

# Run a single test and track result
run_test() {
  local test_name="$1"
  local test_func="test_$test_name"

  if ! declare -f "$test_func" >/dev/null; then
    log_fail "Unknown test: $test_name"
    return 1
  fi

  local start end duration
  start=$(date +%s)

  if $test_func; then
    end=$(date +%s)
    duration=$((end - start))
    TEST_RESULTS[$test_name]="PASS (${duration}s)"
    ((TOTAL_PASSED++))
  else
    end=$(date +%s)
    duration=$((end - start))
    TEST_RESULTS[$test_name]="FAIL (${duration}s)"
    ((TOTAL_FAILED++))
  fi
}

# Print summary
print_summary() {
  echo ""
  echo "=============================================="
  echo "               STRESS TEST SUMMARY"
  echo "=============================================="

  for test_name in "${!TEST_RESULTS[@]}"; do
    local result="${TEST_RESULTS[$test_name]}"
    if [[ $result == PASS* ]]; then
      echo -e "  ${GREEN}$result${NC}  $test_name"
    else
      echo -e "  ${RED}$result${NC}  $test_name"
    fi
  done

  echo "----------------------------------------------"
  echo -e "  Total: ${GREEN}$TOTAL_PASSED passed${NC}, ${RED}$TOTAL_FAILED failed${NC}"
  echo "=============================================="

  if [[ $TOTAL_FAILED -gt 0 ]]; then
    return 1
  fi
  return 0
}

# Main
main() {
  local test_filter="${1:-all}"

  echo ""
  echo "=============================================="
  echo "  FIRECRACKER BUILD SERVICE STRESS TESTS"
  echo "=============================================="
  echo ""

  case "$test_filter" in
  all)
    run_test "parallel"
    run_test "sequential"
    run_test "failure"
    run_test "chain"
    # These are slower, run last
    # run_test "large_input"  # Requires nixpkgs
    run_test "large_output"
    # run_test "long_running"  # Takes 5+ minutes
    ;;
  parallel | sequential | large_input | large_output | long_running | failure | chain)
    run_test "$test_filter"
    ;;
  *)
    echo "Unknown test: $test_filter"
    echo "Available: all, parallel, sequential, large_input, large_output, long_running, failure, chain"
    exit 1
    ;;
  esac

  print_summary
}

main "$@"
