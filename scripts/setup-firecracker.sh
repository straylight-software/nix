#!/usr/bin/env bash
#
# setup-firecracker.sh - Install Firecracker build service components
#
# This script builds and installs the guest kernel and initrd needed for
# the Firecracker build service to function.
#
# Usage:
#   ./scripts/setup-firecracker.sh [--user|--system]
#
# Options:
#   --user    Install to ~/.local/share/nix/firecracker (default)
#   --system  Install to /nix/var/nix/firecracker (requires sudo)
#
# After running this script, the Firecracker build service will be able
# to find the kernel and initrd automatically.

set -euo pipefail

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

info() { echo -e "${GREEN}[INFO]${NC} $*"; }
warn() { echo -e "${YELLOW}[WARN]${NC} $*"; }
error() { echo -e "${RED}[ERROR]${NC} $*" >&2; }

# Parse arguments
INSTALL_MODE="user"
while [[ $# -gt 0 ]]; do
  case $1 in
  --user)
    INSTALL_MODE="user"
    shift
    ;;
  --system)
    INSTALL_MODE="system"
    shift
    ;;
  -h | --help)
    echo "Usage: $0 [--user|--system]"
    echo ""
    echo "Install Firecracker build service guest components."
    echo ""
    echo "Options:"
    echo "  --user    Install to ~/.local/share/nix/firecracker (default)"
    echo "  --system  Install to /nix/var/nix/firecracker (requires sudo)"
    exit 0
    ;;
  *)
    error "Unknown option: $1"
    exit 1
    ;;
  esac
done

# Determine install directory
if [[ "$INSTALL_MODE" == "user" ]]; then
  INSTALL_DIR="${HOME}/.local/share/nix/firecracker"
  SUDO=""
else
  INSTALL_DIR="/nix/var/nix/firecracker"
  SUDO="sudo"
fi

info "Installing Firecracker guest components to $INSTALL_DIR"

# Check for nix
if ! command -v nix &>/dev/null; then
  error "nix command not found. Please install Nix first."
  exit 1
fi

# Build the guest components
info "Building guest kernel and initrd..."
BUILD_OUTPUT=$(nix build .#firecracker-guest --print-out-paths --no-link 2>&1) || {
  error "Failed to build firecracker-guest package"
  echo "$BUILD_OUTPUT"
  exit 1
}

GUEST_PATH="$BUILD_OUTPUT"
info "Built guest components at $GUEST_PATH"

# Verify the build output
if [[ ! -f "$GUEST_PATH/vmlinux" ]]; then
  error "vmlinux not found in build output"
  exit 1
fi

if [[ ! -f "$GUEST_PATH/initrd.img" ]]; then
  error "initrd.img not found in build output"
  exit 1
fi

# Create install directory
info "Creating install directory..."
$SUDO mkdir -p "$INSTALL_DIR"

# Copy files
info "Installing kernel..."
$SUDO cp "$GUEST_PATH/vmlinux" "$INSTALL_DIR/vmlinux"
$SUDO chmod 644 "$INSTALL_DIR/vmlinux"

info "Installing initrd..."
$SUDO cp "$GUEST_PATH/initrd.img" "$INSTALL_DIR/initrd.img"
$SUDO chmod 644 "$INSTALL_DIR/initrd.img"

# Copy init binary for debugging
if [[ -f "$GUEST_PATH/bin/nix-builder-init" ]]; then
  info "Installing init binary (for debugging)..."
  $SUDO mkdir -p "$INSTALL_DIR/bin"
  $SUDO cp "$GUEST_PATH/bin/nix-builder-init" "$INSTALL_DIR/bin/"
  $SUDO chmod 755 "$INSTALL_DIR/bin/nix-builder-init"
fi

# Check for firecracker binary
info "Checking for firecracker binary..."
FC_BIN=""
for path in /usr/bin/firecracker /usr/local/bin/firecracker; do
  if [[ -x "$path" ]]; then
    FC_BIN="$path"
    break
  fi
done

if [[ -z "$FC_BIN" ]]; then
  warn "firecracker binary not found in PATH"
  warn "You may need to install firecracker separately:"
  warn "  - From your package manager (e.g., apt install firecracker)"
  warn "  - From https://github.com/firecracker-microvm/firecracker/releases"
  warn "  - Or set NIX_FIRECRACKER_BIN environment variable"
else
  info "Found firecracker at $FC_BIN"
fi

# Check KVM access
info "Checking KVM access..."
if [[ -e /dev/kvm ]]; then
  if [[ -r /dev/kvm && -w /dev/kvm ]]; then
    info "KVM is accessible"
  else
    warn "KVM device exists but is not accessible"
    warn "Add your user to the 'kvm' group: sudo usermod -aG kvm $USER"
    warn "Then log out and back in"
  fi
else
  warn "/dev/kvm not found - hardware virtualization may not be available"
fi

# Print summary
echo ""
info "Installation complete!"
echo ""
echo "Installed files:"
echo "  $INSTALL_DIR/vmlinux"
echo "  $INSTALL_DIR/initrd.img"
if [[ -f "$INSTALL_DIR/bin/nix-builder-init" ]]; then
  echo "  $INSTALL_DIR/bin/nix-builder-init"
fi
echo ""
echo "To use the Firecracker build service:"
echo "  export NIX_BUILD_SERVICE=firecracker"
echo ""
echo "Or to auto-detect (uses firecracker when daemon unavailable):"
echo "  export NIX_BUILD_SERVICE=auto"
echo ""
if [[ -z "$FC_BIN" ]]; then
  echo "Remember to install the firecracker binary!"
fi
