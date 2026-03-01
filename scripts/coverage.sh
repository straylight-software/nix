#!/usr/bin/env bash
# // straylight // nix // coverage
#
# Generate test coverage reports using llvm-cov.
#
# Usage:
#   ./scripts/coverage.sh              # Run all tests, generate HTML report
#   ./scripts/coverage.sh --report     # Generate report from existing .profraw files
#   ./scripts/coverage.sh --clean      # Remove coverage artifacts
#
# Output:
#   coverage/           - HTML coverage report
#   coverage.profdata   - Merged profile data
#   coverage.txt        - Text summary

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
COVERAGE_DIR="$ROOT_DIR/coverage"
PROFDATA="$ROOT_DIR/coverage.profdata"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

info() { echo -e "${BLUE}[coverage]${NC} $*"; }
success() { echo -e "${GREEN}[coverage]${NC} $*"; }
warn() { echo -e "${YELLOW}[coverage]${NC} $*"; }
error() { echo -e "${RED}[coverage]${NC} $*" >&2; }

# Check for required tools
check_tools() {
  local missing=()
  command -v llvm-profdata >/dev/null 2>&1 || missing+=("llvm-profdata")
  command -v llvm-cov >/dev/null 2>&1 || missing+=("llvm-cov")
  command -v buck2 >/dev/null 2>&1 || missing+=("buck2")

  if [[ ${#missing[@]} -gt 0 ]]; then
    error "Missing required tools: ${missing[*]}"
    error "Run 'nix develop' to enter the dev shell with all tools"
    exit 1
  fi
}

# Clean coverage artifacts
clean() {
  info "Cleaning coverage artifacts..."
  rm -rf "$COVERAGE_DIR" "$PROFDATA" "$ROOT_DIR"/*.profraw "$ROOT_DIR/coverage.txt"
  # Also clean profraw files that tests might have created
  find "$ROOT_DIR" -name "*.profraw" -delete 2>/dev/null || true
  success "Cleaned"
}

# Build tests with coverage instrumentation
build_tests() {
  info "Building tests with coverage instrumentation..."

  # Find all test targets
  local test_targets=(
    "//src/nix/util/tests:..."
    "//src/nix/store/tests:..."
    "//src/nix/tests:..."
  )

  # Build with coverage toolchain
  buck2 build \
    --config cxx.default_toolchain=toolchains//:cxx_coverage \
    "${test_targets[@]}" 2>&1 || {
    error "Failed to build tests with coverage"
    exit 1
  }

  success "Tests built with coverage instrumentation"
}

# Run tests and collect coverage data
run_tests() {
  info "Running tests to collect coverage data..."

  # Set LLVM_PROFILE_FILE to control where .profraw files go
  export LLVM_PROFILE_FILE="$ROOT_DIR/coverage-%p-%m.profraw"

  local test_targets=(
    "//src/nix/util/tests:base-n_test"
    "//src/nix/util/tests:checked-arithmetic_test"
    "//src/nix/util/tests:canon-path_test"
    "//src/nix/util/tests:lru-cache_test"
    "//src/nix/util/tests:strings_test"
    "//src/nix/util/tests:topo-sort_test"
    "//src/nix/util/tests:url_test"
    "//src/nix/tests:link_integrity_test"
    "//src/nix/tests:url_fuzz_test"
    "//src/nix/tests:hash_fuzz_test"
    "//src/nix/tests:github_ssh_fuzz_test"
    "//src/nix/tests:ssh_auth_fuzz_test"
  )

  local failed=0
  for target in "${test_targets[@]}"; do
    info "Running $target..."
    if buck2 run \
      --config cxx.default_toolchain=toolchains//:cxx_coverage \
      "$target" -- 2>&1; then
      success "  PASS: $target"
    else
      warn "  FAIL: $target (continuing...)"
      ((failed++)) || true
    fi
  done

  if [[ $failed -gt 0 ]]; then
    warn "$failed test(s) failed, but continuing with coverage report"
  fi

  success "Test execution complete"
}

# Merge profile data
merge_profiles() {
  info "Merging profile data..."

  local profraw_files=()
  while IFS= read -r -d '' file; do
    profraw_files+=("$file")
  done < <(find "$ROOT_DIR" -name "*.profraw" -print0 2>/dev/null)

  if [[ ${#profraw_files[@]} -eq 0 ]]; then
    error "No .profraw files found. Did you run tests?"
    exit 1
  fi

  info "Found ${#profraw_files[@]} profile file(s)"

  llvm-profdata merge -sparse "${profraw_files[@]}" -o "$PROFDATA" || {
    error "Failed to merge profile data"
    exit 1
  }

  success "Profile data merged to $PROFDATA"
}

# Generate coverage report
generate_report() {
  info "Generating coverage report..."

  if [[ ! -f "$PROFDATA" ]]; then
    error "No profile data found at $PROFDATA"
    error "Run './scripts/coverage.sh' to build and run tests first"
    exit 1
  fi

  # Find test binaries to use as objects
  local binaries=()
  while IFS= read -r -d '' file; do
    # Skip .o files and libraries, only include executables
    if file "$file" | grep -q "executable"; then
      binaries+=("$file")
    fi
  done < <(find "$ROOT_DIR/buck-out" -type f -name "*_test" -print0 2>/dev/null)

  if [[ ${#binaries[@]} -eq 0 ]]; then
    error "No test binaries found in buck-out/"
    exit 1
  fi

  info "Found ${#binaries[@]} binary(ies) for coverage"

  # Build object args
  local obj_args=()
  for bin in "${binaries[@]}"; do
    obj_args+=("-object=$bin")
  done

  # Generate text summary
  info "Generating text summary..."
  llvm-cov report \
    "${binaries[0]}" \
    "${obj_args[@]:1}" \
    -instr-profile="$PROFDATA" \
    -ignore-filename-regex='buck-out|third_party|vendor|test' \
    >"$ROOT_DIR/coverage.txt" 2>/dev/null || true

  # Generate HTML report
  info "Generating HTML report..."
  mkdir -p "$COVERAGE_DIR"
  llvm-cov show \
    "${binaries[0]}" \
    "${obj_args[@]:1}" \
    -instr-profile="$PROFDATA" \
    -format=html \
    -output-dir="$COVERAGE_DIR" \
    -ignore-filename-regex='buck-out|third_party|vendor' \
    -show-line-counts-or-regions \
    -show-instantiations=false \
    2>/dev/null || true

  # Print summary
  echo ""
  success "Coverage report generated!"
  echo ""
  echo "  Text summary: $ROOT_DIR/coverage.txt"
  echo "  HTML report:  $COVERAGE_DIR/index.html"
  echo ""

  # Show quick summary if coverage.txt exists
  if [[ -f "$ROOT_DIR/coverage.txt" ]]; then
    echo "Summary:"
    echo "--------"
    head -20 "$ROOT_DIR/coverage.txt"
  fi
}

# Main
main() {
  cd "$ROOT_DIR"
  check_tools

  case "${1:-}" in
  --clean)
    clean
    ;;
  --report)
    merge_profiles
    generate_report
    ;;
  --help | -h)
    echo "Usage: $0 [--clean|--report|--help]"
    echo ""
    echo "  (no args)  Build tests with coverage, run them, generate report"
    echo "  --clean    Remove coverage artifacts"
    echo "  --report   Generate report from existing .profraw files"
    echo "  --help     Show this help"
    ;;
  *)
    clean
    build_tests
    run_tests
    merge_profiles
    generate_report
    ;;
  esac
}

main "$@"
