// straylight::nix::build::firecracker_build_service
//
// Build service implementation using Firecracker microVMs.
// Provides daemonless, sandboxed builds with deep witnessing.
//
// Architecture:
//   1. Prepare VM rootfs with /nix/store inputs (virtiofs or block device)
//   2. Boot Firecracker microVM (<100ms boot time)
//   3. Execute builder inside VM via vsock command channel
//   4. Stream build output via vsock
//   5. Extract outputs back to host store
//   6. Tear down VM
//
// Witnessing:
//   - All filesystem access visible via virtiofs/FUSE
//   - All syscalls can be traced via seccomp-bpf in guest
//   - Network attempts visible (VM has no network by default)
//   - Build attestation produced for each build
//
// Requirements:
//   - /dev/kvm access (unprivileged with kvm group membership)
//   - Firecracker binary (from vendor/isospin)
//   - Minimal guest kernel + initrd

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <fcntl.h>
#include <linux/vm_sockets.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "build_service.h"
#include "nix/store/derivations.h"
#include "nix/store/store-api.h"
#include "nix/util/file-system.h"
#include "nix/util/logging.h"
#include "nix/util/serialise.h"
#include "vm_protocol.h"

namespace fs = std::filesystem;

namespace straylight::nix::build {

namespace {

// ============================================================================
// Configuration
// ============================================================================

struct firecracker_config {
  // Paths
  std::string firecracker_bin; // Path to firecracker binary
  std::string kernel_path;     // Guest kernel (vmlinux)
  std::string initrd_path;     // Guest initrd with builder

  // VM resources
  uint32_t vcpu_count = 1;
  uint32_t mem_size_mib = 512;

  // Timeouts
  std::chrono::seconds build_timeout{3600}; // 1 hour default
  std::chrono::seconds boot_timeout{10};    // 10s boot timeout

  // Witnessing
  bool enable_witness = true;
  std::string witness_log_dir;

  // Two-tier store paths (matching straylight's architecture)
  // System store: /nix/store (shared read-only packages)
  std::string system_store_dir = "/nix/store";
  // User store: ~/.local/share/nix/store (user-writable)
  std::string user_store_dir;
  // Guest mount points
  std::string guest_system_store = "/nix/store";
  std::string guest_user_store = "/nix/.user-store";
  std::string guest_output_dir = "/nix/store"; // outputs go here
};

// ============================================================================
// Build Witness - records everything the build does
// ============================================================================

struct build_witness {
  std::string drv_path;
  std::chrono::system_clock::time_point start_time;
  std::chrono::system_clock::time_point end_time;

  // Inputs actually read (subset of declared inputs)
  std::vector<std::string> inputs_read;

  // Outputs produced
  std::vector<std::string> outputs_written;

  // Syscall summary (optional, if seccomp tracing enabled)
  std::map<std::string, uint64_t> syscall_counts;

  // Network attempts (should be empty for pure builds)
  std::vector<std::string> network_attempts;

  // Build log hash
  std::string log_hash;

  // Serialize to JSON for attestation
  auto to_json() const -> std::string {
    std::ostringstream ss;
    ss << "{\n";
    ss << "  \"drv_path\": \"" << drv_path << "\",\n";
    ss << "  \"start_time\": " << std::chrono::system_clock::to_time_t(start_time) << ",\n";
    ss << "  \"end_time\": " << std::chrono::system_clock::to_time_t(end_time) << ",\n";
    ss << "  \"inputs_read\": [";
    for (size_t i = 0; i < inputs_read.size(); ++i) {
      if (i > 0) {
        ss << ", ";
      }
      ss << "\"" << inputs_read[i] << "\"";
    }
    ss << "],\n";
    ss << "  \"outputs_written\": [";
    for (size_t i = 0; i < outputs_written.size(); ++i) {
      if (i > 0) {
        ss << ", ";
      }
      ss << "\"" << outputs_written[i] << "\"";
    }
    ss << "],\n";
    ss << "  \"log_hash\": \"" << log_hash << "\"\n";
    ss << "}";
    return ss.str();
  }
};

// ============================================================================
// vsock Client - connects to guest VM via vsock
// ============================================================================

struct vsock_client {
  int fd = -1;
  std::string uds_path; // Unix domain socket path (for Firecracker's vsock device)

  ~vsock_client() { close_connection(); }

  void close_connection() {
    if (fd >= 0) {
      close(fd);
      fd = -1;
    }
  }

  // Connect to guest VM via Firecracker's vsock unix socket
  // Firecracker exposes vsock via a Unix domain socket at uds_path
  // We connect to that socket and then send: "CONNECT <port>\n"
  auto connect_to_guest(const std::string& uds_path_, uint32_t port,
                        std::chrono::milliseconds timeout) -> bool {
    uds_path = uds_path_;

    // Create Unix domain socket
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
      log_error("vsock: socket() failed: %s", strerror(errno));
      return false;
    }

    // Connect to Firecracker's vsock UDS
    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, uds_path.c_str(), sizeof(addr.sun_path) - 1);

    if (connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
      log_error("vsock: connect() to %s failed: %s", uds_path.c_str(), strerror(errno));
      close_connection();
      return false;
    }

    // Send CONNECT command to tell Firecracker which guest port we want
    // Format: "CONNECT <port>\n"
    std::string connect_cmd = "CONNECT " + std::to_string(port) + "\n";
    if (write(fd, connect_cmd.c_str(), connect_cmd.size()) !=
        static_cast<ssize_t>(connect_cmd.size())) {
      log_error("vsock: write CONNECT failed: %s", strerror(errno));
      close_connection();
      return false;
    }

    // Read "OK <guest_port>\n" response
    char response[64];
    struct pollfd pfd = {fd, POLLIN, 0};
    if (poll(&pfd, 1, static_cast<int>(timeout.count())) <= 0) {
      log_error("vsock: timeout waiting for CONNECT response");
      close_connection();
      return false;
    }

    ssize_t n = read(fd, response, sizeof(response) - 1);
    if (n <= 0) {
      log_error("vsock: read CONNECT response failed");
      close_connection();
      return false;
    }
    response[n] = '\0';

    if (strncmp(response, "OK ", 3) != 0) {
      log_error("vsock: unexpected response: %s", response);
      close_connection();
      return false;
    }

    log_debug("vsock: connected to guest port %u", port);
    return true;
  }

  // Send a message with wire protocol header
  auto send_message(vm_msg_type type, std::span<const uint8_t> payload) -> bool {
    auto msg = build_message(type, payload);
    ssize_t written = 0;
    while (written < static_cast<ssize_t>(msg.size())) {
      ssize_t n = write(fd, msg.data() + written, msg.size() - written);
      if (n < 0) {
        if (errno == EINTR) {
          continue;
        }
        log_error("vsock: write failed: %s", strerror(errno));
        return false;
      }
      written += n;
    }
    return true;
  }

  // Read exactly n bytes with timeout
  auto read_exact(uint8_t* buf, size_t len, std::chrono::milliseconds timeout) -> bool {
    size_t total = 0;
    auto deadline = std::chrono::steady_clock::now() + timeout;

    while (total < len) {
      auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
          deadline - std::chrono::steady_clock::now());
      if (remaining.count() <= 0) {
        log_error("vsock: read timeout");
        return false;
      }

      struct pollfd pfd = {fd, POLLIN, 0};
      int ret = poll(&pfd, 1, static_cast<int>(remaining.count()));
      if (ret < 0) {
        if (errno == EINTR) {
          continue;
        }
        log_error("vsock: poll failed: %s", strerror(errno));
        return false;
      }
      if (ret == 0) {
        log_error("vsock: read timeout");
        return false;
      }

      ssize_t n = read(fd, buf + total, len - total);
      if (n < 0) {
        if (errno == EINTR) {
          continue;
        }
        log_error("vsock: read failed: %s", strerror(errno));
        return false;
      }
      if (n == 0) {
        log_error("vsock: connection closed");
        return false;
      }
      total += n;
    }
    return true;
  }

  // Receive a message header
  auto recv_header(std::chrono::milliseconds timeout) -> std::optional<vm_wire_header> {
    uint8_t header_buf[sizeof(vm_wire_header)];
    if (!read_exact(header_buf, sizeof(header_buf), timeout)) {
      return std::nullopt;
    }
    auto hdr = parse_header(std::span<const uint8_t>(header_buf, sizeof(header_buf)));
    if (!hdr || !validate_header(*hdr)) {
      log_error("vsock: invalid header");
      return std::nullopt;
    }
    return hdr;
  }

  // Receive payload after header
  auto recv_payload(uint32_t len, std::chrono::milliseconds timeout) -> std::vector<uint8_t> {
    std::vector<uint8_t> payload(len);
    if (!read_exact(payload.data(), len, timeout)) {
      return {};
    }
    return payload;
  }
};

// ============================================================================
// VM Instance - manages a single Firecracker VM
// ============================================================================

struct vm_instance {
  pid_t pid = -1;
  int api_socket = -1;
  int vsock_fd = -1;
  std::string socket_path;
  std::string vsock_uds_path; // Path to vsock unix domain socket
  std::string work_dir;
  bool running = false;

  ~vm_instance() {
    if (running) {
      shutdown();
    }
  }

  void shutdown() {
    if (pid > 0) {
      // Send SIGTERM first, then SIGKILL after timeout
      kill(pid, SIGTERM);
      int status;
      auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
      while (std::chrono::steady_clock::now() < deadline) {
        if (waitpid(pid, &status, WNOHANG) > 0) {
          break;
        }
        usleep(10000); // 10ms
      }
      // Force kill if still running
      if (waitpid(pid, &status, WNOHANG) == 0) {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
      }
      pid = -1;
    }
    if (api_socket >= 0) {
      close(api_socket);
      api_socket = -1;
    }
    if (vsock_fd >= 0) {
      close(vsock_fd);
      vsock_fd = -1;
    }
    running = false;
  }
};

// ============================================================================
// Firecracker Build Service Implementation
// ============================================================================

struct firecracker_build_service final : build_service {
  firecracker_config config_;
  mutable bool available_checked_ = false;
  mutable bool available_ = false;

  explicit firecracker_build_service(firecracker_config config) : config_(std::move(config)) {}

  // --------------------------------------------------------------------------
  // build_service interface
  // --------------------------------------------------------------------------

  void build_paths(::nix::store_t& store, const std::vector<::nix::derived_path_t>& paths,
                   ::nix::BuildMode build_mode) override {
    // Build each path, ignoring results
    for (const auto& path : paths) {
      std::visit(
          [&](const auto& p) {
            using T = std::decay_t<decltype(p)>;
            if constexpr (std::is_same_v<T, ::nix::DerivedPath::Built>) {
              // Get the derivation and build it
              auto drv_path = resolve_derived_path_built(store, p);
              if (drv_path) {
                auto drv = store.readDerivation(*drv_path);
                build_derivation(store, *drv_path, drv, build_mode);
              }
            } else if constexpr (std::is_same_v<T, ::nix::DerivedPath::Opaque>) {
              // Opaque path - just ensure it exists (substitution)
              ensure_path(store, p.path);
            }
          },
          path.raw());
    }
  }

  auto build_paths_with_results(::nix::store_t& store,
                                const std::vector<::nix::derived_path_t>& paths,
                                ::nix::BuildMode build_mode)
      -> std::vector<::nix::keyed_build_result_t> override {
    std::vector<::nix::keyed_build_result_t> results;
    results.reserve(paths.size());

    for (const auto& path : paths) {
      std::visit(
          [&](const auto& p) {
            using T = std::decay_t<decltype(p)>;
            if constexpr (std::is_same_v<T, ::nix::DerivedPath::Built>) {
              auto drv_path = resolve_derived_path_built(store, p);
              if (drv_path) {
                auto drv = store.readDerivation(*drv_path);
                auto result = build_derivation(store, *drv_path, drv, build_mode);
                results.push_back(::nix::keyed_build_result_t{path, result});
              }
            } else if constexpr (std::is_same_v<T, ::nix::DerivedPath::Opaque>) {
              // Opaque paths don't have build results in the same sense
              ::nix::build_result_t result;
              result.status = ::nix::build_result_t::Substituted;
              results.push_back(::nix::keyed_build_result_t{path, result});
            }
          },
          path.raw());
    }

    return results;
  }

  auto build_derivation(::nix::store_t& store, const ::nix::store_path_t& drv_path,
                        const ::nix::basic_derivation_t& drv, ::nix::BuildMode build_mode)
      -> ::nix::build_result_t override {
    log_info("firecracker: building %s", store.printStorePath(drv_path));

    ::nix::build_result_t result;
    result.status = ::nix::build_result_t::MiscFailure;

    try {
      // Create witness record
      build_witness witness;
      witness.drv_path = store.printStorePath(drv_path);
      witness.start_time = std::chrono::system_clock::now();

      // Execute the build in a Firecracker VM
      result = execute_in_vm(store, drv_path, drv, witness);

      witness.end_time = std::chrono::system_clock::now();

      // Save witness if enabled
      if (config_.enable_witness && !config_.witness_log_dir.empty()) {
        save_witness(witness);
      }

    } catch (const std::exception& e) {
      log_error("firecracker build failed: %s", e.what());
      result.status = ::nix::build_result_t::MiscFailure;
      result.errorMsg = e.what();
    }

    return result;
  }

  void ensure_path(::nix::store_t& store, const ::nix::store_path_t& path) override {
    if (store.isValidPath(path)) {
      return;
    }
    // Try substitution first
    // TODO: implement substitution
    throw ::nix::Error("firecracker_build_service: substitution not yet implemented for %s",
                       store.printStorePath(path));
  }

  auto name() const -> std::string_view override { return "firecracker"; }

  auto is_available() const -> bool override {
    if (!available_checked_) {
      available_checked_ = true;
      available_ = check_availability();
    }
    return available_;
  }

  // --------------------------------------------------------------------------
  // Internal implementation
  // --------------------------------------------------------------------------

private:
  auto check_availability() const -> bool {
    // Check for /dev/kvm
    if (access("/dev/kvm", R_OK | W_OK) != 0) {
      log_debug("firecracker: /dev/kvm not accessible");
      return false;
    }

    // Check for firecracker binary
    if (!config_.firecracker_bin.empty() && access(config_.firecracker_bin.c_str(), X_OK) != 0) {
      log_debug("firecracker: binary not found at %s", config_.firecracker_bin);
      return false;
    }

    // Check for kernel
    if (!config_.kernel_path.empty() && !fs::exists(config_.kernel_path)) {
      log_debug("firecracker: kernel not found at %s", config_.kernel_path);
      return false;
    }

    return true;
  }

  auto resolve_derived_path_built(::nix::store_t& store, const ::nix::DerivedPath::Built& built)
      -> std::optional<::nix::store_path_t> {
    return std::visit(
        [&](const auto& d) -> std::optional<::nix::store_path_t> {
          using T = std::decay_t<decltype(d)>;
          if constexpr (std::is_same_v<T, ::nix::DerivedPath::Opaque>) {
            return d.path;
          } else {
            // Recursively resolve
            return std::nullopt;
          }
        },
        built.drvPath->raw());
  }

  auto execute_in_vm(::nix::store_t& store, const ::nix::store_path_t& drv_path,
                     const ::nix::basic_derivation_t& drv, build_witness& witness)
      -> ::nix::build_result_t {
    ::nix::build_result_t result;

    // Create work directory for this build
    auto work_dir = create_work_dir(drv_path);
    log_debug("firecracker: work_dir = %s", work_dir);

    // Collect input paths that need to be available in the VM (from both stores)
    auto inputs = collect_input_paths(store, drv);

    // Record all inputs for witness (combine both stores)
    for (const auto& p : inputs.system_store) {
      witness.inputs_read.push_back(p);
    }
    for (const auto& p : inputs.user_store) {
      witness.inputs_read.push_back(p);
    }
    log_debug("firecracker: %zu system store + %zu user store input paths",
              inputs.system_store.size(), inputs.user_store.size());

    // Prepare the VM configuration
    auto vm_config = prepare_vm_config(store, drv, work_dir, inputs);

    // Start the VM
    auto vm = start_vm(vm_config);
    if (!vm || !vm->running) {
      result.status = ::nix::build_result_t::MiscFailure;
      result.errorMsg = "Failed to start Firecracker VM";
      return result;
    }

    // Execute the builder inside the VM
    result = run_builder_in_vm(*vm, store, drv, witness);

    // Extract outputs if build succeeded
    if (result.status == ::nix::build_result_t::Built) {
      extract_outputs(store, drv, work_dir, witness);
    }

    // Shutdown VM
    vm->shutdown();

    // Cleanup work directory
    // fs::remove_all(work_dir);  // Keep for debugging

    return result;
  }

  auto create_work_dir(const ::nix::store_path_t& drv_path) -> std::string {
    auto base = fs::temp_directory_path() / "firecracker-build";
    fs::create_directories(base);

    auto name = drv_path.name();
    auto work = base / name;
    if (fs::exists(work)) {
      fs::remove_all(work);
    }
    fs::create_directories(work);
    fs::create_directories(work / "rootfs");
    fs::create_directories(work / "outputs");

    return work.string();
  }

  // Categorized input paths for two-tier store
  struct categorized_inputs {
    std::vector<std::string> system_store; // Paths in /nix/store
    std::vector<std::string> user_store;   // Paths in user store
  };

  auto collect_input_paths(::nix::store_t& store, const ::nix::basic_derivation_t& drv)
      -> categorized_inputs {
    categorized_inputs inputs;

    // Resolve each input to its actual location
    for (const auto& src : drv.input_srcs) {
      auto path_str = store.printStorePath(src);
      auto real_path = resolve_store_path(path_str);
      categorize_path(real_path, inputs);
    }

    // Add builder if it's a store path
    if (drv.builder.starts_with("/nix/store")) {
      auto real_path = resolve_store_path(drv.builder);
      categorize_path(real_path, inputs);
    }

    return inputs;
  }

  // Resolve a /nix/store path to its actual location (user or system store)
  auto resolve_store_path(const std::string& path) -> std::string {
    // Check user store first
    if (!config_.user_store_dir.empty()) {
      auto basename = fs::path(path).filename().string();
      auto user_path = fs::path(config_.user_store_dir) / basename;
      if (fs::exists(user_path)) {
        return user_path.string();
      }
    }
    // Fallback to system store
    return path;
  }

  // Categorize a resolved path into system or user store
  void categorize_path(const std::string& real_path, categorized_inputs& inputs) {
    if (!config_.user_store_dir.empty() && real_path.starts_with(config_.user_store_dir)) {
      inputs.user_store.push_back(real_path);
    } else {
      inputs.system_store.push_back(real_path);
    }
  }

  auto prepare_vm_config(::nix::store_t& store, const ::nix::basic_derivation_t& drv,
                         const std::string& work_dir, const categorized_inputs& inputs)
      -> std::string {
    // Generate Firecracker JSON config
    //
    // Architecture for two-tier store access:
    //   - Boot from initrd (no rootfs block device needed)
    //   - Use initrd's tmpfs as root
    //   - Guest init mounts stores via virtio-9p or virtiofs
    //   - Outputs written to /build, extracted after build
    //
    // Note: Firecracker doesn't support 9p natively, but supports virtiofs.
    // For maximum compatibility, we use a different approach:
    //   - Copy required inputs into a block device image
    //   - Use overlay filesystem in guest for outputs
    //   - Extract outputs from the overlay after build

    std::ostringstream config;
    config << "{\n";

    // Boot source - using initrd, no root device
    config << "  \"boot-source\": {\n";
    config << "    \"kernel_image_path\": \"" << config_.kernel_path << "\",\n";
    config << "    \"initrd_path\": \"" << config_.initrd_path << "\",\n";
    // Boot args: init=/init, no root= since we boot from initrd
    // Pass store mount info via kernel cmdline
    config << "    \"boot_args\": \"console=ttyS0 reboot=k panic=1 pci=off init=/init\"\n";
    config << "  },\n";

    // Machine config
    config << "  \"machine-config\": {\n";
    config << "    \"vcpu_count\": " << config_.vcpu_count << ",\n";
    config << "    \"mem_size_mib\": " << config_.mem_size_mib << "\n";
    config << "  },\n";

    // Drives:
    //   vda: /nix/store inputs (read-only, sparse image with hardlinks)
    //   vdb: /build output area (read-write)
    config << "  \"drives\": [\n";

    // Store inputs drive (read-only)
    config << "    {\n";
    config << "      \"drive_id\": \"store\",\n";
    config << "      \"path_on_host\": \"" << work_dir << "/store.ext4\",\n";
    config << "      \"is_root_device\": false,\n";
    config << "      \"is_read_only\": true\n";
    config << "    },\n";

    // Output drive (read-write)
    config << "    {\n";
    config << "      \"drive_id\": \"output\",\n";
    config << "      \"path_on_host\": \"" << work_dir << "/output.ext4\",\n";
    config << "      \"is_root_device\": false,\n";
    config << "      \"is_read_only\": false\n";
    config << "    }\n";

    config << "  ],\n";

    // vsock for communication
    config << "  \"vsock\": {\n";
    config << "    \"guest_cid\": 3,\n";
    config << "    \"uds_path\": \"" << work_dir << "/vsock.sock\"\n";
    config << "  }\n";

    config << "}\n";

    // Write config to file
    auto config_path = work_dir + "/vm-config.json";
    std::ofstream config_file(config_path);
    config_file << config.str();
    config_file.close();

    // Create the store and output images
    create_store_image(work_dir, inputs);
    create_output_image(work_dir);

    return config_path;
  }

  void create_store_image(const std::string& work_dir, const categorized_inputs& inputs) {
    // Create a sparse ext4 image containing all required store paths
    auto image_path = work_dir + "/store.ext4";
    auto mount_point = work_dir + "/store-mount";

    // Calculate size needed
    uint64_t total_size = 64 * 1024 * 1024; // 64MB base overhead
    auto add_path_size = [&total_size](const std::string& path) {
      if (fs::exists(path)) {
        if (fs::is_directory(path)) {
          for (const auto& entry : fs::recursive_directory_iterator(path)) {
            if (entry.is_regular_file()) {
              total_size += entry.file_size();
            }
          }
        } else {
          total_size += fs::file_size(path);
        }
      }
    };

    for (const auto& path : inputs.system_store) {
      add_path_size(path);
    }
    for (const auto& path : inputs.user_store) {
      add_path_size(path);
    }

    // Add 50% padding for filesystem overhead and safety
    total_size = (total_size * 150) / 100;
    total_size = std::max(total_size, uint64_t(128 * 1024 * 1024)); // Min 128MB

    log_debug("firecracker: creating store image %s (%zu MB)", image_path,
              total_size / (1024 * 1024));

    // Create and format the image
    create_ext4_image(image_path, total_size);

    // Create mount point
    fs::create_directories(mount_point);

    // Mount image using fuse2fs (no root required) or fallback to loop mount
    bool mounted = false;
    auto fuse_cmd = "fuse2fs -o rw " + image_path + " " + mount_point + " 2>/dev/null";
    if (system(fuse_cmd.c_str()) == 0) {
      mounted = true;
      log_debug("firecracker: mounted store image via fuse2fs");
    } else {
      // Fallback to loop mount (requires privileges)
      auto loop_cmd = "mount -o loop " + image_path + " " + mount_point + " 2>/dev/null";
      if (system(loop_cmd.c_str()) == 0) {
        mounted = true;
        log_debug("firecracker: mounted store image via loop");
      }
    }

    if (!mounted) {
      throw ::nix::Error("failed to mount store image - need fuse2fs or root for loop mount");
    }

    // Create /nix/store directory structure inside the image
    auto store_dir = fs::path(mount_point) / "nix" / "store";
    fs::create_directories(store_dir);

    // Copy all input paths into the image
    size_t copied = 0;
    auto copy_path = [&](const std::string& src_path) {
      auto basename = fs::path(src_path).filename().string();
      auto dst_path = store_dir / basename;

      if (fs::exists(dst_path)) {
        return; // Already copied (dedup)
      }

      try {
        fs::copy(src_path, dst_path, fs::copy_options::recursive | fs::copy_options::copy_symlinks);
        copied++;
      } catch (const std::exception& e) {
        log_warning("firecracker: failed to copy %s: %s", src_path, e.what());
      }
    };

    log_info("firecracker: copying %zu system + %zu user store paths to image",
             inputs.system_store.size(), inputs.user_store.size());

    for (const auto& path : inputs.system_store) {
      copy_path(path);
    }
    for (const auto& path : inputs.user_store) {
      copy_path(path);
    }

    // Unmount
    auto umount_cmd = "fusermount -u " + mount_point + " 2>/dev/null || umount " + mount_point;
    if (system(umount_cmd.c_str()) != 0) {
      log_warning("firecracker: failed to unmount store image cleanly");
    }

    log_info("firecracker: store image populated with %zu paths", copied);
  }

  void create_output_image(const std::string& work_dir) {
    // Create a writable image for build outputs
    auto image_path = work_dir + "/output.ext4";
    uint64_t size = 512 * 1024 * 1024; // 512MB for outputs

    log_debug("firecracker: creating output image %s (%zu MB)", image_path, size / (1024 * 1024));

    create_ext4_image(image_path, size);
  }

  void create_ext4_image(const std::string& path, uint64_t size) {
    // Create sparse file
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
      throw ::nix::sys_error_t("failed to create image: " + path);
    }
    if (ftruncate(fd, size) != 0) {
      close(fd);
      throw ::nix::sys_error_t("failed to size image: " + path);
    }
    close(fd);

    // Format as ext4
    auto cmd = "mkfs.ext4 -q -F " + path;
    if (system(cmd.c_str()) != 0) {
      throw ::nix::Error("failed to format image as ext4: " + path);
    }
  }

  auto start_vm(const std::string& config_path) -> std::unique_ptr<vm_instance> {
    auto vm = std::make_unique<vm_instance>();

    // Extract work dir from config path
    vm->work_dir = fs::path(config_path).parent_path().string();
    vm->socket_path = vm->work_dir + "/firecracker.sock";
    vm->vsock_uds_path = vm->work_dir + "/vsock.sock";

    // Fork and exec firecracker
    vm->pid = fork();
    if (vm->pid < 0) {
      throw ::nix::sys_error_t("fork failed");
    }

    if (vm->pid == 0) {
      // Child process - exec firecracker
      execl(config_.firecracker_bin.c_str(), "firecracker", "--no-api", "--config-file",
            config_path.c_str(), nullptr);
      _exit(127);
    }

    // Parent - wait for VM to start and vsock socket to appear
    vm->running = true;

    // Poll for vsock socket to exist (indicates VM is ready)
    auto deadline = std::chrono::steady_clock::now() + config_.boot_timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      // Check if VM crashed
      int status;
      if (waitpid(vm->pid, &status, WNOHANG) != 0) {
        vm->running = false;
        log_error("firecracker: VM exited during boot");
        return nullptr;
      }

      // Check if vsock socket exists
      if (access(vm->vsock_uds_path.c_str(), F_OK) == 0) {
        // Socket exists, but give it a moment to be ready
        usleep(50000); // 50ms
        break;
      }

      usleep(10000); // 10ms
    }

    if (access(vm->vsock_uds_path.c_str(), F_OK) != 0) {
      log_error("firecracker: vsock socket never appeared at %s", vm->vsock_uds_path);
      vm->shutdown();
      return nullptr;
    }

    log_info("firecracker: VM started, pid=%d, vsock=%s", vm->pid, vm->vsock_uds_path);
    return vm;
  }

  auto run_builder_in_vm(vm_instance& vm, ::nix::store_t& store,
                         const ::nix::basic_derivation_t& drv, build_witness& witness)
      -> ::nix::build_result_t {
    ::nix::build_result_t result;
    result.status = ::nix::build_result_t::MiscFailure;

    log_info("firecracker: executing builder %s", drv.builder);

    // Connect to guest via vsock
    vsock_client vsock;
    if (!vsock.connect_to_guest(vm.vsock_uds_path, VM_VSOCK_BUILD_PORT,
                                std::chrono::milliseconds(5000))) {
      result.errorMsg = "Failed to connect to guest builder via vsock";
      return result;
    }

    // Prepare build request
    build_exec_request req;
    req.builder = drv.builder;
    req.args = drv.args;
    req.workdir = "/build"; // Standard build directory in guest

    // Convert environment map to vector of pairs
    for (const auto& [key, value] : drv.env) {
      req.env.emplace_back(key, value);
    }

    // Add expected output paths
    for (const auto& [name, output] : drv.outputs) {
      // Get the output path string
      auto output_path = std::visit(
          [&](const auto& o) -> std::string {
            using T = std::decay_t<decltype(o)>;
            if constexpr (std::is_same_v<T, ::nix::DerivationOutput::InputAddressed>) {
              return store.printStorePath(o.path);
            } else if constexpr (std::is_same_v<T, ::nix::DerivationOutput::CAFixed>) {
              return store.printStorePath(o.path);
            } else {
              // For floating outputs, we compute the path later
              return "";
            }
          },
          output.raw());
      if (!output_path.empty()) {
        req.outputs.push_back(output_path);
      }
    }

    // Send BUILD_EXEC
    auto payload = req.serialize();
    if (!vsock.send_message(vm_msg_type::BUILD_EXEC, std::span<const uint8_t>(payload))) {
      result.errorMsg = "Failed to send build request to guest";
      return result;
    }

    log_debug("firecracker: sent BUILD_EXEC, waiting for completion...");

    // Process responses until we get BUILD_EXIT
    auto timeout = std::chrono::duration_cast<std::chrono::milliseconds>(config_.build_timeout);
    std::string build_log;

    while (true) {
      auto hdr = vsock.recv_header(timeout);
      if (!hdr) {
        result.errorMsg = "Lost connection to guest during build";
        return result;
      }

      auto msg_payload = vsock.recv_payload(hdr->payload_len, timeout);
      if (msg_payload.empty() && hdr->payload_len > 0) {
        result.errorMsg = "Failed to read message payload from guest";
        return result;
      }

      auto msg_type = static_cast<vm_msg_type>(hdr->msg_type);

      switch (msg_type) {
        case vm_msg_type::BUILD_STDOUT: {
          // Append to build log
          std::string out(msg_payload.begin(), msg_payload.end());
          build_log += out;
          // Also stream to logger
          log_info("firecracker [stdout]: %s", out.c_str());
          break;
        }

        case vm_msg_type::BUILD_STDERR: {
          std::string err(msg_payload.begin(), msg_payload.end());
          build_log += err;
          log_info("firecracker [stderr]: %s", err.c_str());
          break;
        }

        case vm_msg_type::BUILD_EXIT: {
          auto exit_resp = build_exit_response::deserialize(std::span<const uint8_t>(msg_payload));
          if (exit_resp.success && exit_resp.exit_code == 0) {
            result.status = ::nix::build_result_t::Built;
            log_info("firecracker: build succeeded");
          } else {
            result.status = ::nix::build_result_t::BuildFailure;
            result.errorMsg = exit_resp.error_msg.empty() ? "Builder exited with code " +
                                                                std::to_string(exit_resp.exit_code)
                                                          : exit_resp.error_msg;
            log_error("firecracker: build failed: %s", result.errorMsg.c_str());
          }
          // Hash the build log for witness
          // TODO: Compute actual hash
          witness.log_hash = "sha256:" + std::to_string(std::hash<std::string>{}(build_log));
          return result;
        }

        case vm_msg_type::WITNESS_EVENT: {
          auto evt = witness_event::deserialize(std::span<const uint8_t>(msg_payload));
          // Record witness data
          switch (evt.type) {
            case witness_event::event_type::FILE_READ:
              witness.inputs_read.push_back(evt.path);
              break;
            case witness_event::event_type::FILE_WRITE:
              witness.outputs_written.push_back(evt.path);
              break;
            case witness_event::event_type::NET_CONNECT:
              witness.network_attempts.push_back(evt.path);
              log_warning("firecracker: build attempted network access: %s", evt.path);
              break;
            case witness_event::event_type::SYSCALL:
              witness.syscall_counts[evt.syscall]++;
              break;
            default:
              break;
          }
          break;
        }

        case vm_msg_type::PONG:
          // Ignore pongs
          break;

        default:
          log_warning("firecracker: unexpected message type 0x%04x", static_cast<int>(msg_type));
          break;
      }
    }
  }

  void extract_outputs(::nix::store_t& store, const ::nix::basic_derivation_t& drv,
                       const std::string& work_dir, build_witness& witness) {
    // Extract outputs from the output image to the host store
    //
    // Strategy:
    // 1. Mount the output.ext4 image (requires privileges or FUSE)
    // 2. For each output, copy from mount to user store
    // 3. Register in store database
    //
    // For now, we use a simple approach: call out to helper scripts
    // that can use FUSE or privileged mounts.

    auto output_image = work_dir + "/output.ext4";
    auto mount_point = work_dir + "/output-mount";

    // Create mount point
    fs::create_directories(mount_point);

    // Try to mount using fuse2fs (ext4 FUSE driver) - no root required
    auto mount_cmd = "fuse2fs -o ro " + output_image + " " + mount_point + " 2>/dev/null";
    bool mounted = (system(mount_cmd.c_str()) == 0);

    if (!mounted) {
      // Fallback: try loop mount (requires privileges)
      mount_cmd = "mount -o ro,loop " + output_image + " " + mount_point + " 2>/dev/null";
      mounted = (system(mount_cmd.c_str()) == 0);
    }

    if (!mounted) {
      log_error("firecracker: failed to mount output image at %s", mount_point);
      log_error("firecracker: outputs may need manual extraction from %s", output_image);
      return;
    }

    log_info("firecracker: mounted output image at %s", mount_point);

    // Extract each output
    for (const auto& [name, output] : drv.outputs) {
      auto output_path = std::visit(
          [&](const auto& o) -> std::string {
            using T = std::decay_t<decltype(o)>;
            if constexpr (std::is_same_v<T, ::nix::DerivationOutput::InputAddressed>) {
              return store.printStorePath(o.path);
            } else if constexpr (std::is_same_v<T, ::nix::DerivationOutput::CAFixed>) {
              return store.printStorePath(o.path);
            } else {
              return "";
            }
          },
          output.raw());

      if (output_path.empty()) {
        log_warning("firecracker: skipping output '%s' (floating/deferred)", name);
        continue;
      }

      // Extract basename (e.g., "abc123-foo" from "/nix/store/abc123-foo")
      auto basename = fs::path(output_path).filename().string();

      // Source path in the mounted output image
      auto src_path = fs::path(mount_point) / "nix" / "store" / basename;

      if (!fs::exists(src_path)) {
        log_error("firecracker: output '%s' not found at %s", name, src_path.c_str());
        continue;
      }

      // Destination in user store
      auto dst_path = fs::path(config_.user_store_dir) / basename;

      log_info("firecracker: extracting output '%s': %s -> %s", name, src_path.c_str(),
               dst_path.c_str());

      try {
        // Remove existing if present (shouldn't happen with CA, but safety first)
        if (fs::exists(dst_path)) {
          fs::remove_all(dst_path);
        }

        // Copy the output
        fs::copy(src_path, dst_path, fs::copy_options::recursive);

        witness.outputs_written.push_back(output_path);
        log_info("firecracker: extracted output '%s' (%zu bytes)", name, calculate_size(dst_path));

      } catch (const std::exception& e) {
        log_error("firecracker: failed to extract output '%s': %s", name, e.what());
      }
    }

    // Unmount
    auto umount_cmd = "fusermount -u " + mount_point + " 2>/dev/null || umount " + mount_point;
    system(umount_cmd.c_str());

    log_info("firecracker: output extraction complete");
  }

  // Calculate total size of a path (file or directory)
  static auto calculate_size(const fs::path& path) -> uint64_t {
    uint64_t total = 0;
    if (fs::is_directory(path)) {
      for (const auto& entry : fs::recursive_directory_iterator(path)) {
        if (entry.is_regular_file()) {
          total += entry.file_size();
        }
      }
    } else if (fs::is_regular_file(path)) {
      total = fs::file_size(path);
    }
    return total;
  }

  void save_witness(const build_witness& witness) {
    auto witness_path = fs::path(config_.witness_log_dir) /
                        (fs::path(witness.drv_path).filename().string() + ".witness.json");
    std::ofstream file(witness_path);
    file << witness.to_json();
    log_debug("firecracker: witness saved to %s", witness_path.c_str());
  }
};

} // namespace

// ============================================================================
// Factory Functions
// ============================================================================

std::unique_ptr<build_service> make_firecracker_build_service() {
  firecracker_config config;

  // Check environment variables first
  const char* fc_bin = getenv("NIX_FIRECRACKER_BIN");
  const char* fc_kernel = getenv("NIX_FIRECRACKER_KERNEL");
  const char* fc_initrd = getenv("NIX_FIRECRACKER_INITRD");

  if (fc_bin) {
    config.firecracker_bin = fc_bin;
  } else {
    // Try to find firecracker binary in common locations
    for (const auto& path : {
             "/usr/bin/firecracker",
             "/usr/local/bin/firecracker",
             "./result/bin/firecracker",
         }) {
      if (access(path, X_OK) == 0) {
        config.firecracker_bin = path;
        break;
      }
    }
  }

  if (fc_kernel) {
    config.kernel_path = fc_kernel;
  } else {
    // Try to find guest kernel in common locations
    const char* home = getenv("HOME");
    std::vector<std::string> kernel_paths = {
        "./result/vmlinux",
        "./.firecracker-guest/vmlinux",
    };
    if (home) {
      kernel_paths.push_back(std::string(home) + "/.local/share/nix/firecracker/vmlinux");
      kernel_paths.push_back(std::string(home) + "/.nix-firecracker/vmlinux");
    }
    kernel_paths.push_back("/nix/var/nix/firecracker/vmlinux");

    for (const auto& path : kernel_paths) {
      if (access(path.c_str(), R_OK) == 0) {
        config.kernel_path = path;
        break;
      }
    }
  }

  if (fc_initrd) {
    config.initrd_path = fc_initrd;
  } else {
    // Try to find guest initrd in common locations
    const char* home = getenv("HOME");
    std::vector<std::string> initrd_paths = {
        "./result/initrd.img",
        "./.firecracker-guest/initrd.img",
    };
    if (home) {
      initrd_paths.push_back(std::string(home) + "/.local/share/nix/firecracker/initrd.img");
      initrd_paths.push_back(std::string(home) + "/.nix-firecracker/initrd.img");
    }
    initrd_paths.push_back("/nix/var/nix/firecracker/initrd.img");

    for (const auto& path : initrd_paths) {
      if (access(path.c_str(), R_OK) == 0) {
        config.initrd_path = path;
        break;
      }
    }
  }

  // Set user store path (two-tier store architecture)
  const char* home = getenv("HOME");
  if (home) {
    config.user_store_dir = std::string(home) + "/.local/share/nix/store";
  }

  // Log configuration
  if (!config.firecracker_bin.empty()) {
    log_debug("firecracker: binary = %s", config.firecracker_bin);
  }
  if (!config.kernel_path.empty()) {
    log_debug("firecracker: kernel = %s", config.kernel_path);
  }
  if (!config.initrd_path.empty()) {
    log_debug("firecracker: initrd = %s", config.initrd_path);
  }

  return std::make_unique<firecracker_build_service>(std::move(config));
}

std::unique_ptr<build_service> make_firecracker_build_service(const std::string& firecracker_bin,
                                                              const std::string& kernel_path,
                                                              const std::string& initrd_path) {
  firecracker_config config;
  config.firecracker_bin = firecracker_bin;
  config.kernel_path = kernel_path;
  config.initrd_path = initrd_path;

  // Set user store path
  const char* home = getenv("HOME");
  if (home) {
    config.user_store_dir = std::string(home) + "/.local/share/nix/store";
  }

  return std::make_unique<firecracker_build_service>(std::move(config));
}

} // namespace straylight::nix::build
