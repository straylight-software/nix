# Complete NixOS VFIO GPU Passthrough Guide

## Your Hardware

| Device | PCI Address | Vendor:Device | IOMMU Group | Current State |
|--------|-------------|---------------|-------------|---------------|
| **NVIDIA RTX PRO 6000 Blackwell** | `0000:01:00.0` | `10de:2bb1` | 13 | `vfio-pci` ✓ |
| **NVIDIA HD Audio** | `0000:01:00.1` | `10de:22e8` | 13 | **no driver** ✗ |
| **AMD Radeon iGPU** | `0000:72:00.0` | `1002:13c0` | (separate) | `amdgpu` ✓ |

## Current Problem

Your current NixOS config has **conflicting settings**:
- NVIDIA drivers are loaded (`nvidia`, `nvidia_drm`, `nvidia_modeset`, `nvidia_uvm`)
- `vfio-pci` is also loaded
- GPU is bound to `vfio-pci` (good), but audio device has **no driver** (bad)
- IOMMU group 13 is not viable because not ALL devices are bound to `vfio-pci`

---

## NixOS Configuration

Add this to your `configuration.nix` (or hardware module):

```nix
{ config, lib, pkgs, ... }:

{
  # ═══════════════════════════════════════════════════════════════════════════
  # IOMMU AND KERNEL PARAMETERS
  # ═══════════════════════════════════════════════════════════════════════════
  
  boot.kernelParams = [
    "amd_iommu=on"
    "iommu=pt"
    # Remove any nvidia-drm.modeset=1 or nvidia-drm.fbdev=1 from your config
  ];

  # ═══════════════════════════════════════════════════════════════════════════
  # VFIO MODULES - Load early in initrd to claim GPU before nvidia
  # ═══════════════════════════════════════════════════════════════════════════
  
  boot.initrd.kernelModules = [
    "vfio_pci"
    "vfio"
    "vfio_iommu_type1"
  ];

  # ═══════════════════════════════════════════════════════════════════════════
  # BIND RTX PRO 6000 + AUDIO TO VFIO-PCI AT BOOT
  # ═══════════════════════════════════════════════════════════════════════════
  
  boot.extraModprobeConfig = ''
    # Bind NVIDIA RTX PRO 6000 and its audio device to vfio-pci
    options vfio-pci ids=10de:2bb1,10de:22e8
    
    # Ensure vfio-pci loads before nvidia/nouveau would try to claim the device
    softdep nvidia pre: vfio-pci
    softdep nvidia_drm pre: vfio-pci
    softdep nvidia_modeset pre: vfio-pci
    softdep nvidia_uvm pre: vfio-pci
    softdep nouveau pre: vfio-pci
    softdep snd_hda_intel pre: vfio-pci
  '';

  # ═══════════════════════════════════════════════════════════════════════════
  # BLACKLIST NVIDIA DRIVERS (GPU is dedicated to passthrough)
  # ═══════════════════════════════════════════════════════════════════════════
  
  boot.blacklistedKernelModules = [
    "nvidia"
    "nvidia_drm"
    "nvidia_modeset"
    "nvidia_uvm"
    "nouveau"
    "nvidiafb"
  ];

  # ═══════════════════════════════════════════════════════════════════════════
  # HOST DISPLAY - AMD RADEON iGPU
  # ═══════════════════════════════════════════════════════════════════════════
  
  # Use only AMD for display (remove nvidia from videoDrivers if present)
  services.xserver.videoDrivers = lib.mkForce [ "amdgpu" ];
  
  # Disable NVIDIA hardware config if you have it enabled
  hardware.nvidia.modesetting.enable = lib.mkForce false;
  # hardware.nvidia.open = lib.mkForce false;  # uncomment if you have this

  # ═══════════════════════════════════════════════════════════════════════════
  # VFIO DEVICE PERMISSIONS
  # ═══════════════════════════════════════════════════════════════════════════
  
  services.udev.extraRules = ''
    # Allow kvm group to access VFIO devices
    SUBSYSTEM=="vfio", OWNER="root", GROUP="kvm", MODE="0660"
  '';

  # Ensure your user is in kvm group
  # users.users.YOUR_USERNAME.extraGroups = [ "kvm" ];

  # ═══════════════════════════════════════════════════════════════════════════
  # KVM/VIRTUALIZATION
  # ═══════════════════════════════════════════════════════════════════════════
  
  boot.kernelModules = [ "kvm-amd" ];
  virtualisation.libvirtd.enable = true;  # optional, for libvirt/QEMU
}
```

---

## What to Remove from Your Current Config

Based on your kernel command line, you currently have NVIDIA settings that conflict:

```
nvidia-drm.modeset=1 nvidia-drm.fbdev=1 nvidia.NVreg_OpenRmEnableUnsupportedGpus=1
```

**Remove or disable:**
- `hardware.nvidia.enable = true;` (or set to `false`)
- `hardware.nvidia.modesetting.enable = true;`
- `hardware.nvidia.open = true;`
- Any `services.xserver.videoDrivers = [ ... "nvidia" ... ];`
- Any nvidia kernel params

---

## Apply and Reboot

```bash
# 1. Edit your configuration
sudo nano /etc/nixos/configuration.nix

# 2. Rebuild (don't switch yet - review first)
sudo nixos-rebuild dry-build

# 3. If satisfied, switch and reboot
sudo nixos-rebuild switch
sudo reboot
```

---

## Verification After Reboot

```bash
# 1. Check both NVIDIA devices are bound to vfio-pci
lspci -nnk -s 01:00
# Should show:
#   01:00.0 ... Kernel driver in use: vfio-pci
#   01:00.1 ... Kernel driver in use: vfio-pci

# 2. Check VFIO modules loaded, nvidia NOT loaded
lsmod | grep -E 'vfio|nvidia'
# Should show vfio modules, NO nvidia modules

# 3. Check VFIO device exists
ls -la /dev/vfio/
# Should show /dev/vfio/13 and /dev/vfio/vfio

# 4. Check IOMMU group is complete
./scripts/vfio-status.sh
# Both 01:00.0 and 01:00.1 should show [vfio-pci]

# 5. Check AMD iGPU is handling display
lspci -nnk -s 72:00.0
# Should show: Kernel driver in use: amdgpu
```

---

## Test GPU Passthrough with Firecracker

After reboot and verification:

```bash
# 1. Commit the VFIO integration code (if not done)
cd /home/b7r6/src/vendor/isospin
git add -A
git commit -m "feat(firecracker): Add VFIO PCI device passthrough support"

# 2. Build Firecracker
buck2 build //firecracker/src/firecracker:firecracker

# 3. Run with GPU
sudo ./buck-out/v2/gen/root/*/firecracker/src/firecracker/__firecracker__/firecracker \
  --config-file .vm-assets/vm-config-gpu.json \
  --api-sock /tmp/fc-gpu.sock
```

---

## Alternative: Test with Cloud Hypervisor First

Cloud Hypervisor has more mature VFIO support. Good for validating the setup:

```bash
# Setup network
sudo ./scripts/setup-network.sh

# Create VM image (if not done)
./scripts/create-gpu-image.sh

# Boot with GPU
sudo ./scripts/boot-ch-gpu.sh 0000:01:00.0

# SSH into VM
ssh -i .vm-assets/vm_key root@172.16.0.2

# In VM: check GPU
lspci | grep -i nvidia
nvidia-smi
```

---

## Troubleshooting

### Problem: Display doesn't work after reboot

Your display should now be on the AMD iGPU. If it's not:
1. Check your monitor is connected to the AMD iGPU output (motherboard ports, not GPU)
2. Check BIOS settings - make sure iGPU is enabled

### Problem: `nvidia` modules still loading

Check for NixOS options that auto-enable nvidia:
```bash
grep -r nvidia /etc/nixos/
nixos-option hardware.nvidia
```

### Problem: Only one device bound to vfio-pci

Check the `options vfio-pci ids=` line has BOTH IDs:
```bash
cat /sys/module/vfio_pci/parameters/ids
# Should show: 10de:2bb1,10de:22e8
```

### Problem: VFIO group not viable

```bash
# Check all devices in group 13
ls /sys/kernel/iommu_groups/13/devices/
# Should only show 0000:01:00.0 and 0000:01:00.1

# Check their drivers
for d in /sys/kernel/iommu_groups/13/devices/*; do
  echo -n "$(basename $d): "
  readlink $d/driver | xargs basename 2>/dev/null || echo "none"
done
# Both must show: vfio-pci
```

---

## Summary Checklist

- [ ] Add VFIO config to `configuration.nix`
- [ ] Remove/disable nvidia config from `configuration.nix`
- [ ] Run `sudo nixos-rebuild switch`
- [ ] Reboot
- [ ] Verify both `01:00.0` and `01:00.1` show `vfio-pci`
- [ ] Verify `nvidia` modules not loaded
- [ ] Verify `/dev/vfio/13` exists
- [ ] Commit VFIO code to git
- [ ] Test with Cloud Hypervisor or Firecracker
