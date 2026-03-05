# vendor/isospin/nix/vendor.nix
#
# Generates the Rust vendor directory for isospin (Firecracker + gpu-broker).
# Based on isospin-microvm's flake-modules/main.nix vendor module.
#
# Usage:
#   rustVendor = import ./vendor.nix { inherit pkgs; };
#   # Then symlink: ln -sf $rustVendor vendor/isospin/third-party/rust/vendor
#
{ pkgs }:
let
  cargoLock = ../third-party/rust/Cargo.lock;
  fixupsDir = ../third-party/rust/fixups;
  sed = "${pkgs.gnused}/bin/sed";

  # Output hashes for git dependencies
  outputHashes = {
    "micro_http-0.1.0" = "sha256-XemdzwS25yKWEXJcRX2l6QzD7lrtroMeJNOUEWGR7WQ=";
  };

  vendorBase = pkgs.rustPlatform.importCargoLock {
    lockFile = cargoLock;
    inherit outputHashes;
  };

  # Fetch acpi_tables from rust-vmm (needed by Cloud Hypervisor)
  acpiTablesSrc = pkgs.fetchFromGitHub {
    owner = "rust-vmm";
    repo = "acpi_tables";
    rev = "e08a3f0b0a59b98859dbf59f5aa7fd4d2eb4018a";
    hash = "sha256-ykg3UFX/8r8uYlbBxHfRHElH7qGHQIfCJ8VTjxOD+Hk=";
  };

  # Patched vendor with vm-memory 0.18 fixes
  vendorPatched =
    pkgs.runCommand "isospin-rust-vendor-patched"
      {
        inherit fixupsDir acpiTablesSrc;
        nativeBuildInputs = [
          pkgs.perl
          pkgs.gnused
        ];
      }
      ''
        mkdir -p $out
        cp -rL ${vendorBase}/* $out/
        chmod -R u+w $out

        # Add acpi_tables from rust-vmm git (needed by Cloud Hypervisor)
        mkdir -p $out/acpi_tables-0.1.0
        cp -r $acpiTablesSrc/* $out/acpi_tables-0.1.0/
        chmod -R u+w $out/acpi_tables-0.1.0
        echo "Added acpi_tables from rust-vmm git"

        # Cargo.toml version constraint patches
        if [ -d $out/linux-loader-0.13.2 ]; then
          ${sed} -i 's/<=0.17.1/<=0.18.0/g' $out/linux-loader-0.13.2/Cargo.toml
          echo "Patched linux-loader-0.13.2 Cargo.toml"
        fi

        for crate in virtio-queue-0.16.0 vhost-user-backend-0.20.0 vfio-ioctls-0.5.2 vfio_user-0.1.2; do
          if [ -d $out/$crate ]; then
            ${sed} -i 's/"0.16"/">=0.16, <=0.18"/g' $out/$crate/Cargo.toml
            echo "Patched $crate Cargo.toml"
          fi
        done

        # rustix extern crate errno fix
        if [ -d $out/rustix-1.1.3 ]; then
          for file in io/errno.rs fs/dir.rs event/syscalls.rs fs/syscalls.rs process/syscalls.rs; do
            filepath="$out/rustix-1.1.3/src/backend/libc/$file"
            if [ -f "$filepath" ]; then
              ${sed} -i '0,/^use /{s/^use /extern crate errno as libc_errno;\n\nuse /}' "$filepath"
              echo "Patched rustix: $file"
            fi
          done
        fi

        # linux-loader vm-memory 0.18 GuestMemoryBackend bounds
        if [ -d $out/linux-loader-0.13.2 ]; then
          for file in \
            $out/linux-loader-0.13.2/src/configurator/fdt.rs \
            $out/linux-loader-0.13.2/src/configurator/mod.rs \
            $out/linux-loader-0.13.2/src/configurator/x86_64/linux.rs \
            $out/linux-loader-0.13.2/src/configurator/x86_64/pvh.rs \
            $out/linux-loader-0.13.2/src/loader/bzimage/mod.rs \
            $out/linux-loader-0.13.2/src/loader/elf/mod.rs \
            $out/linux-loader-0.13.2/src/loader/mod.rs \
            $out/linux-loader-0.13.2/src/loader/pe/mod.rs
          do
            if [ -f "$file" ]; then
              ${sed} -i 's/M: GuestMemory,$/M: GuestMemory + GuestMemoryBackend,/g' "$file"
              ${sed} -i 's/M: GuestMemory;$/M: GuestMemory + GuestMemoryBackend;/g' "$file"
              ${sed} -i 's/<F, M: GuestMemory>/<F, M: GuestMemory + GuestMemoryBackend>/g' "$file"
              ${sed} -i 's/<M: GuestMemory>/<M: GuestMemory + GuestMemoryBackend>/g' "$file"
            fi
          done

          # Add GuestMemoryBackend to imports
          ${sed} -i 's/use vm_memory::{Bytes, GuestMemory};/use vm_memory::{Bytes, GuestMemory, GuestMemoryBackend};/g' \
            $out/linux-loader-0.13.2/src/configurator/fdt.rs
          ${sed} -i 's/use vm_memory::{Address, ByteValued, GuestAddress, GuestMemory};/use vm_memory::{Address, ByteValued, GuestAddress, GuestMemory, GuestMemoryBackend};/g' \
            $out/linux-loader-0.13.2/src/configurator/mod.rs
          ${sed} -i 's/use vm_memory::{Bytes, GuestMemory};/use vm_memory::{Bytes, GuestMemory, GuestMemoryBackend};/g' \
            $out/linux-loader-0.13.2/src/configurator/x86_64/linux.rs
          ${sed} -i 's/use vm_memory::{ByteValued, Bytes, GuestMemory};/use vm_memory::{ByteValued, Bytes, GuestMemory, GuestMemoryBackend};/g' \
            $out/linux-loader-0.13.2/src/configurator/x86_64/pvh.rs
          ${sed} -i 's/GuestMemory, GuestUsize, ReadVolatile};/GuestMemory, GuestMemoryBackend, GuestUsize, ReadVolatile};/g' \
            $out/linux-loader-0.13.2/src/loader/bzimage/mod.rs \
            $out/linux-loader-0.13.2/src/loader/elf/mod.rs \
            $out/linux-loader-0.13.2/src/loader/mod.rs \
            $out/linux-loader-0.13.2/src/loader/pe/mod.rs

          echo "Patched linux-loader-0.13.2 for vm-memory 0.18"
        fi

        # vm-memory-0.17.2: Export both GuestMemoryBackend AND the new GuestMemory trait
        # The shim aliases GuestMemoryBackend as GuestMemory for compat.
        # We also need to export the NEW GuestMemory trait as IommuMemory for code that
        # needs to call physical_memory()
        if [ -d $out/vm-memory-0.17.2 ]; then
          # In guest_memory.rs, export GuestMemoryBackend and GuestMemory (new) as IommuMemory
          ${sed} -i 's/GuestMemoryBackend as GuestMemory,$/GuestMemoryBackend as GuestMemory, GuestMemoryBackend, GuestMemory as IommuMemory,/g' \
            $out/vm-memory-0.17.2/src/guest_memory.rs
          # In lib.rs, re-export these
          ${sed} -i 's/Error as GuestMemoryError, FileOffset, GuestAddress, GuestAddressSpace, GuestMemory,$/Error as GuestMemoryError, FileOffset, GuestAddress, GuestAddressSpace, GuestMemory, GuestMemoryBackend, IommuMemory,/g' \
            $out/vm-memory-0.17.2/src/lib.rs
          echo "Patched vm-memory-0.17.2 to export GuestMemoryBackend and IommuMemory"
        fi

        # vhost-0.15.0: Use physical_memory() to access GuestMemoryBackend methods
        # The issue is GuestAddressSpace::M implements the NEW GuestMemory trait (IommuMemory),
        # but the methods address_in_range() and get_host_address() are on GuestMemoryBackend.
        # The blanket impl means any GuestMemoryBackend also implements IommuMemory with
        # physical_memory() returning Some(self).
        if [ -d $out/vhost-0.15.0 ]; then
          # Import IommuMemory for physical_memory() access
          ${sed} -i 's/use vm_memory::{Address, GuestAddress, GuestAddressSpace, GuestMemory, GuestUsize};/use vm_memory::{Address, GuestAddress, GuestAddressSpace, GuestMemory, GuestUsize, IommuMemory};/g' \
            $out/vhost-0.15.0/src/vhost_kern/mod.rs

          # Patch is_valid(): Replace m.address_in_range(v) with IommuMemory::physical_memory(&*m).unwrap().address_in_range(v)
          ${sed} -i 's/!m\.address_in_range(v)/!IommuMemory::physical_memory(\&*m).unwrap().address_in_range(v)/g' \
            $out/vhost-0.15.0/src/vhost_kern/mod.rs

          # Patch to_vhost_vring_addr(): Need to get physical memory first
          # Original: mem.memory().get_host_address(GuestAddress(...))
          # New: IommuMemory::physical_memory(&*mem.memory()).unwrap().get_host_address(GuestAddress(...))
          
          # Use perl for multi-line replacements - need -0777 to slurp whole file
          perl -i -0777 -pe 's/let desc_addr = mem\n            \.memory\(\)\n            \.get_host_address/let desc_addr = IommuMemory::physical_memory(\&*mem.memory()).unwrap()\n            .get_host_address/g' \
            $out/vhost-0.15.0/src/vhost_kern/mod.rs
          perl -i -0777 -pe 's/let avail_addr = mem\n            \.memory\(\)\n            \.get_host_address/let avail_addr = IommuMemory::physical_memory(\&*mem.memory()).unwrap()\n            .get_host_address/g' \
            $out/vhost-0.15.0/src/vhost_kern/mod.rs
          perl -i -0777 -pe 's/let used_addr = mem\n            \.memory\(\)\n            \.get_host_address/let used_addr = IommuMemory::physical_memory(\&*mem.memory()).unwrap()\n            .get_host_address/g' \
            $out/vhost-0.15.0/src/vhost_kern/mod.rs

          echo "Patched vhost-0.15.0 for vm-memory 0.18"
        fi

        # Apply overlay files from fixups
        if [ -d "$fixupsDir" ]; then
          for fixup in $fixupsDir/*/overlay; do
            if [ -d "$fixup" ]; then
              crate=$(basename $(dirname $fixup))
              for vendor_crate in $out/$crate-*; do
                if [ -d "$vendor_crate" ]; then
                  mkdir -p "$vendor_crate/out"
                  cp -r "$fixup"/* "$vendor_crate/out/" 2>/dev/null || true
                  echo "Applied overlay for $crate"
                fi
              done
            fi
          done
        fi

        echo "Vendor patching complete"
      '';
in
vendorPatched
