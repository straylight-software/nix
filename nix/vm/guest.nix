# Guest kernel and initrd for Firecracker build VMs
#
# Builds:
# 1. Minimal Linux kernel (vmlinux) with vsock, virtio, overlay FS
# 2. Static nix-builder-init binary (musl)
# 3. CPIO initrd containing init + minimal userspace
#
# The resulting derivation provides:
#   $out/vmlinux      - Uncompressed kernel for Firecracker (~25MB stripped)
#   $out/initrd.img   - CPIO archive with init system (~35KB)
#   $out/bin/nix-builder-init - Static init binary (for debugging)
#
# Size targets for embedding in binary:
#   - vmlinux: ~25-30MB (stripped, no debug info)
#   - initrd.img: ~35KB (gzip compressed)
#   - Total: ~30MB embedded data section

{ pkgs }:

let
  # Use standard kernel with required features for Firecracker
  # Note: vmlinux is ~600MB with debug info, but we strip it to ~30MB at build time
  kernel = pkgs.linuxPackages_6_1.kernel.override {
    structuredExtraConfig = with pkgs.lib.kernel; {
      # Ensure virtio support for Firecracker
      VIRTIO = yes;
      VIRTIO_PCI = yes;
      VIRTIO_MMIO = yes;
      VIRTIO_BLK = yes;
      VIRTIO_CONSOLE = yes;

      # Enable vsock for host communication
      VSOCKETS = yes;
      VIRTIO_VSOCKETS = yes;
      VIRTIO_VSOCKETS_COMMON = yes;

      # Enable overlay filesystem for /nix/store
      OVERLAY_FS = yes;

      # Enable ext4 for store images (likely already enabled)
      EXT4_FS = yes;
    };
  };

  # Build the static init binary using pkgsStatic
  nixBuilderInit = pkgs.pkgsStatic.stdenv.mkDerivation {
    pname = "nix-builder-init";
    version = "0.1.0";

    src = ../../src/straylight/nix/build/guest;

    # pkgsStatic already provides musl-based static toolchain
    buildPhase = ''
      $CC -static -O2 -Wall -Wextra -o nix-builder-init nix-builder-init.c
    '';

    installPhase = ''
      mkdir -p $out/bin
      install -m 755 nix-builder-init $out/bin/
    '';
  };

  # Build the initrd
  # Note: Device nodes cannot be created in nix sandbox (no CAP_MKNOD)
  # The init program will mount devtmpfs to populate /dev at runtime
  initrd =
    pkgs.runCommand "firecracker-initrd"
      {
        nativeBuildInputs = [
          pkgs.cpio
          pkgs.gzip
          pkgs.binutils # for strip
        ];
      }
      ''
        mkdir -p $out/bin
        mkdir -p $TMPDIR/initrd/{bin,dev,etc,proc,sys,tmp,nix/store,build,output,nix-work}

        # Copy init binary
        cp ${nixBuilderInit}/bin/nix-builder-init $TMPDIR/initrd/init
        chmod 755 $TMPDIR/initrd/init

        # Note: Device nodes will be created by devtmpfs at runtime
        # The init program mounts devtmpfs to /dev before accessing devices

        # Basic /etc files
        echo "root:x:0:0:root:/root:/bin/sh" > $TMPDIR/initrd/etc/passwd
        echo "root:x:0:" > $TMPDIR/initrd/etc/group

        # Create initrd CPIO archive
        cd $TMPDIR/initrd
        find . -print0 | cpio --null -o --format=newc | gzip -9 > $out/initrd.img

        # Copy kernel and strip debug info for smaller size
        # vmlinux is in the dev output, not the main output
        cp ${kernel.dev}/vmlinux $out/vmlinux
        chmod +w $out/vmlinux
        strip --strip-debug $out/vmlinux || true

        # Also copy init binary for debugging
        cp ${nixBuilderInit}/bin/nix-builder-init $out/bin/

        # Report sizes
        echo "=== Artifact sizes ==="
        ls -lh $out/vmlinux $out/initrd.img
      '';

in
{
  # Main output
  firecracker-guest = initrd;

  # Individual components for debugging/testing
  inherit kernel nixBuilderInit;
}
