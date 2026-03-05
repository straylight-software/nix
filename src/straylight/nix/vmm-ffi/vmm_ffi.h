// straylight::nix::vmm-ffi
//
// C header for Firecracker VMM FFI.
// This header is manually maintained to match src/lib.rs.
//
// TODO: Auto-generate with cbindgen

#ifndef STRAYLIGHT_NIX_VMM_FFI_H
#define STRAYLIGHT_NIX_VMM_FFI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Error Codes
// ============================================================================

#define VMM_OK 0
#define VMM_ERR_INVALID_ARG (-1)
#define VMM_ERR_KVM_INIT (-2)
#define VMM_ERR_VM_CREATE (-3)
#define VMM_ERR_BOOT (-4)
#define VMM_ERR_DEVICE (-5)
#define VMM_ERR_VSOCK (-6)
#define VMM_ERR_INTERNAL (-99)

// ============================================================================
// Types
// ============================================================================

/// Opaque handle to a VM instance
typedef struct VmmHandle VmmHandle;

/// VM configuration
typedef struct VmmConfig {
  /// Path to kernel image (vmlinux)
  const char* kernel_path;
  /// Path to initrd image (may be NULL)
  const char* initrd_path;
  /// Kernel command line (may be NULL)
  const char* kernel_cmdline;
  /// Number of vCPUs
  uint32_t vcpu_count;
  /// Memory size in MiB
  uint32_t mem_size_mib;
} VmmConfig;

/// Block device configuration
typedef struct VmmBlockDevice {
  /// Device ID (e.g., "store", "output")
  const char* drive_id;
  /// Path to block device file
  const char* path;
  /// Read-only flag (non-zero = read-only)
  int is_read_only;
} VmmBlockDevice;

/// vsock configuration
typedef struct VmmVsockConfig {
  /// Guest CID
  uint32_t guest_cid;
  /// Path to Unix domain socket
  const char* uds_path;
} VmmVsockConfig;

/// Compression formats for embedded data
#define VMM_COMPRESS_NONE 0
#define VMM_COMPRESS_ZSTD 2

/// VM configuration with embedded kernel/initrd data
/// Used for creating VMs from data embedded in the binary
typedef struct VmmEmbeddedConfig {
  /// Pointer to kernel image data (vmlinux, possibly compressed)
  const void* kernel_data;
  /// Size of kernel data in bytes
  size_t kernel_size;
  /// Pointer to initrd image data (CPIO, possibly compressed)
  const void* initrd_data;
  /// Size of initrd data in bytes
  size_t initrd_size;
  /// Kernel command line (may be NULL)
  const char* kernel_cmdline;
  /// Number of vCPUs
  uint32_t vcpu_count;
  /// Memory size in MiB
  uint32_t mem_size_mib;
  /// Compression format for kernel: 0=none, 2=zstd
  uint32_t kernel_compression;
  /// Compression format for initrd: 0=none, 2=zstd
  uint32_t initrd_compression;
} VmmEmbeddedConfig;

// ============================================================================
// Functions
// ============================================================================

/// Create a new VM instance with the given configuration.
///
/// Returns a handle on success, NULL on failure.
/// The error code is written to `error_out` if non-NULL.
VmmHandle* vmm_create(const VmmConfig* config, int* error_out);

/// Create a new VM instance from embedded kernel/initrd data.
///
/// This function creates memfds from the provided data and configures the VM.
/// The data can be compressed with zstd (kernel_compression/initrd_compression = 2).
///
/// Returns a handle on success, NULL on failure.
/// The error code is written to `error_out` if non-NULL.
VmmHandle* vmm_create_from_embedded(const VmmEmbeddedConfig* config, int* error_out);

/// Add a block device to the VM.
///
/// Must be called before vmm_start().
/// Returns VMM_OK on success.
int vmm_add_block_device(VmmHandle* handle, const VmmBlockDevice* device);

/// Configure vsock for the VM.
///
/// Must be called before vmm_start().
/// Returns VMM_OK on success.
int vmm_configure_vsock(VmmHandle* handle, const VmmVsockConfig* config);

/// Start the VM.
///
/// This boots the VM and starts vCPU threads.
/// The VM runs asynchronously; use vmm_wait() to block until completion.
/// Returns VMM_OK on success.
int vmm_start(VmmHandle* handle);

/// Wait for the VM to exit.
///
/// Blocks until the VM shuts down or an error occurs.
/// Returns the VM exit code.
int vmm_wait(VmmHandle* handle);

/// Shutdown the VM.
///
/// Forcibly terminates the VM if it's still running.
/// Returns VMM_OK on success.
int vmm_shutdown(VmmHandle* handle);

/// Destroy the VM handle and free resources.
///
/// The handle is invalid after this call.
void vmm_destroy(VmmHandle* handle);

/// Get the vsock file descriptor for communication.
///
/// Returns the FD on success, -1 on failure.
/// This FD can be used to connect to guest services.
int vmm_get_vsock_fd(VmmHandle* handle);

/// Get a human-readable error message for the given error code.
const char* vmm_strerror(int error);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // STRAYLIGHT_NIX_VMM_FFI_H
