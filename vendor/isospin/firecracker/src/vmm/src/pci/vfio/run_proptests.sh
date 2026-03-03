#!/bin/bash
# Standalone property test runner for VFIO module
# This runs the property tests with high iteration counts to thoroughly test invariants

set -e

cd "$(dirname "$0")"

echo "=== Running VFIO Property Tests ==="
echo ""

# Use buck2 to run tests
cd /home/b7r6/src/vendor/isospin

echo "Running all VFIO property tests..."
./buck2 test //firecracker/src/vmm:vmm-test -- \
	--exact \
	pci::vfio::proptests \
	--test-threads=4

echo ""
echo "=== Property Tests Complete ==="
