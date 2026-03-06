// straylight::nix::build::firecracker_build_service
//
// Build service implementation using Firecracker microVMs.
// Provides daemonless, sandboxed builds with deep witnessing.
//
// Architecture:
//   1. Prepare VM rootfs with /nix/store inputs (virtiofs or block device)
//   2. Boot Firecracker microVM (<100ms boot time) - EMBEDDED via vmm-ffi
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
//   - Minimal guest kernel + initrd
//
// Note: The Firecracker VMM is embedded directly via vmm-ffi, eliminating
// the need for a separate firecracker binary and avoiding seccomp issues.

#include <atomic>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <thread>

#include <fcntl.h>
#include <linux/vm_sockets.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "straylight/nix/build/vmm_ffi.h"

#include "build_service.h"
#include "embedded_guest.h"
#include "nix/store/build/derivation-env-desugar.h"
#include "nix/store/derivation-options.h"
#include "nix/store/derivations.h"
#include "nix/store/realisation.h"
#include "nix/store/store-api.h"
#include "nix/store/store-open.h"
#include "nix/util/experimental-features.h"
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
  // Paths (VMM is embedded via vmm-ffi, no separate firecracker binary needed)
  std::string kernel_path; // Guest kernel (vmlinux)
  std::string initrd_path; // Guest initrd with builder

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
    // Format inputs array
    std::string inputs_json;
    for (size_t i = 0; i < inputs_read.size(); ++i) {
      if (i > 0) {
        inputs_json += ", ";
      }
      inputs_json += std::format("\"{}\"", inputs_read[i]);
    }

    // Format outputs array
    std::string outputs_json;
    for (size_t i = 0; i < outputs_written.size(); ++i) {
      if (i > 0) {
        outputs_json += ", ";
      }
      outputs_json += std::format("\"{}\"", outputs_written[i]);
    }

    return std::format(
        R"({{
  "drv_path": "{}",
  "start_time": {},
  "end_time": {},
  "inputs_read": [{}],
  "outputs_written": [{}],
  "log_hash": "{}"
}})",
        drv_path, std::chrono::system_clock::to_time_t(start_time),
        std::chrono::system_clock::to_time_t(end_time), inputs_json, outputs_json, log_hash);
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

    // Retry loop - guest might still be booting when we first try
    auto deadline = std::chrono::steady_clock::now() + timeout;
    int attempt = 0;

    while (std::chrono::steady_clock::now() < deadline) {
      attempt++;

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
        log_debug("vsock: connect() attempt %d to %s failed: %s", attempt, uds_path.c_str(),
                  strerror(errno));
        close_connection();
        usleep(100000); // 100ms
        continue;
      }

      // Send CONNECT command to tell Firecracker which guest port we want
      // Format: "CONNECT <port>\n"
      std::string connect_cmd = "CONNECT " + std::to_string(port) + "\n";
      if (write(fd, connect_cmd.c_str(), connect_cmd.size()) !=
          static_cast<ssize_t>(connect_cmd.size())) {
        log_debug("vsock: write CONNECT attempt %d failed: %s", attempt, strerror(errno));
        close_connection();
        usleep(100000);
        continue;
      }

      // Read "OK <guest_port>\n" response
      // Firecracker returns this once it successfully connects to the guest listener
      char response[64];
      struct pollfd pfd = {fd, POLLIN, 0};
      if (poll(&pfd, 1, 500) <= 0) { // 500ms per attempt
        log_debug("vsock: timeout on attempt %d waiting for CONNECT response", attempt);
        close_connection();
        continue;
      }

      ssize_t n = read(fd, response, sizeof(response) - 1);
      if (n <= 0) {
        log_debug("vsock: read CONNECT response attempt %d failed", attempt);
        close_connection();
        usleep(100000);
        continue;
      }
      response[n] = '\0';

      if (strncmp(response, "OK ", 3) != 0) {
        log_debug("vsock: unexpected response on attempt %d: %s", attempt, response);
        close_connection();
        usleep(100000);
        continue;
      }

      log_debug("vsock: connected to guest port %u after %d attempts", port, attempt);
      return true;
    }

    log_error("vsock: failed to connect to guest port %u after %d attempts", port, attempt);
    return false;
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
// VM Instance - manages a single Firecracker VM (embedded via vmm-ffi)
// ============================================================================

struct vm_instance {
  VmmHandle* vmm_handle = nullptr; // Embedded VMM handle
  int vsock_fd = -1;
  std::string vsock_uds_path; // Path to vsock unix domain socket
  std::string work_dir;
  bool running = false;

  ~vm_instance() {
    if (running) {
      shutdown();
    }
  }

  void shutdown() {
    if (vmm_handle) {
      // Shutdown and destroy the embedded VMM
      vmm_shutdown(vmm_handle);
      vmm_destroy(vmm_handle);
      vmm_handle = nullptr;
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
            if constexpr (std::is_same_v<T, ::nix::derived_path_t::Built>) {
              // Get the derivation and build it
              auto drv_path = resolve_derived_path_built(store, p);
              if (drv_path) {
                auto drv = store.read_derivation(*drv_path);
                // Ensure all input derivation outputs are available (substitute if needed)
                ensure_input_drv_outputs(store, drv);
                // Resolve input_drvs to input_srcs so we have all input paths
                auto resolved = drv.try_resolve(store);
                if (resolved) {
                  build_derivation(store, *drv_path, *resolved, build_mode);
                } else {
                  log_error("firecracker: failed to resolve derivation %s",
                            store.printStorePath(*drv_path));
                }
              }
            } else if constexpr (std::is_same_v<T, ::nix::derived_path_t::opaque_t>) {
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
            if constexpr (std::is_same_v<T, ::nix::derived_path_t::Built>) {
              auto drv_path = resolve_derived_path_built(store, p);
              if (drv_path) {
                auto drv = store.read_derivation(*drv_path);
                // Ensure all input derivation outputs are available (substitute if needed)
                ensure_input_drv_outputs(store, drv);
                // Resolve input_drvs to input_srcs so we have all input paths
                auto resolved = drv.try_resolve(store);
                if (resolved) {
                  auto result = build_derivation(store, *drv_path, *resolved, build_mode);
                  results.push_back(::nix::keyed_build_result_t{result, path});
                } else {
                  log_error("firecracker: failed to resolve derivation %s",
                            store.printStorePath(*drv_path));
                  ::nix::build_result_t fail_result;
                  fail_result.inner = ::nix::build_result_t::Failure{
                      .status = ::nix::build_result_t::Failure::MiscFailure,
                      .errorMsg = "Failed to resolve derivation inputs"};
                  results.push_back(::nix::keyed_build_result_t{fail_result, path});
                }
              }
            } else if constexpr (std::is_same_v<T, ::nix::derived_path_t::opaque_t>) {
              // Opaque paths don't have build results in the same sense
              ::nix::build_result_t result;
              result.inner = ::nix::build_result_t::Success{
                  .status = ::nix::build_result_t::Success::Substituted};
              results.push_back(::nix::keyed_build_result_t{result, path});
            }
          },
          path.raw());
    }

    return results;
  }

  auto build_derivation(::nix::store_t& store, const ::nix::store_path_t& drv_path,
                        const ::nix::basic_derivation_t& drv, ::nix::BuildMode /*build_mode*/)
      -> ::nix::build_result_t override {
    log_info("firecracker: building %s", store.printStorePath(drv_path));

    ::nix::build_result_t result;

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
      result.inner = ::nix::build_result_t::Failure{
          .status = ::nix::build_result_t::Failure::MiscFailure, .errorMsg = e.what()};
    }

    return result;
  }

  void ensure_path(::nix::store_t& store, const ::nix::store_path_t& path) override {
    if (store.isValidPath(path)) {
      return;
    }

    // Try substitution from configured binary caches
    log_info("firecracker: substituting %s", store.printStorePath(path));

    auto substituters = ::nix::get_default_substituters();
    if (substituters.empty()) {
      throw ::nix::Error("firecracker: no substituters configured, cannot fetch %s",
                         store.printStorePath(path));
    }

    for (const auto& sub : substituters) {
      try {
        // Query path info from substituter
        auto info = sub->queryPathInfo(path);

        // Recursively ensure all references are available first
        for (const auto& ref : info->references) {
          if (ref != path) { // Skip self-references
            ensure_path(store, ref);
          }
        }

        // Copy the path from substituter to local store
        log_debug("firecracker: copying %s from %s", store.printStorePath(path),
                  sub->config.getHumanReadableURI());
        ::nix::copy_store_path(*sub, store, path, ::nix::NoRepair,
                               sub->config.isTrusted ? ::nix::NoCheckSigs : ::nix::CheckSigs);

        log_info("firecracker: substituted %s", store.printStorePath(path));
        return;

      } catch (::nix::InvalidPath&) {
        // This substituter doesn't have it, try next
        continue;
      } catch (::nix::SubstituterDisabled&) {
        continue;
      } catch (::nix::Error& e) {
        log_debug("firecracker: substituter %s failed: %s", sub->config.getHumanReadableURI(),
                  e.what());
        continue;
      }
    }

    throw ::nix::Error("firecracker: path %s not available and no substituter has it",
                       store.printStorePath(path));
  }

  // Ensure all input derivation outputs are available (substitute if needed)
  // This must be called before try_resolve() which requires outputs to exist
  void ensure_input_drv_outputs(::nix::store_t& store, const ::nix::derivation_t& drv) {
    for (const auto& [input_drv_path, input_node] : drv.input_drvs.map) {
      // For each output requested from this input derivation
      for (const auto& output_name : input_node.value) {
        // Get the expected output path
        auto input_drv = store.read_derivation(input_drv_path);
        auto output_it = input_drv.outputs.find(output_name);
        if (output_it == input_drv.outputs.end()) {
          throw ::nix::Error("firecracker: derivation %s does not have output '%s'",
                             store.printStorePath(input_drv_path), output_name);
        }

        // Get the output path based on derivation output type
        auto output_path_opt = std::visit(
            [&](const auto& o) -> std::optional<::nix::store_path_t> {
              using T = std::decay_t<decltype(o)>;
              if constexpr (std::is_same_v<T, ::nix::derivation_output_t::InputAddressed>) {
                return o.path;
              } else if constexpr (std::is_same_v<T, ::nix::derivation_output_t::CAFixed>) {
                return o.path(store, input_drv.name, output_name);
              } else if constexpr (std::is_same_v<T, ::nix::derivation_output_t::CAFloating>) {
                // Floating CA - check if we have a realisation for this output
                auto drv_hashes = ::nix::static_output_hashes(store, input_drv);
                auto hash_it = drv_hashes.find(output_name);
                if (hash_it != drv_hashes.end()) {
                  auto drv_output = ::nix::DrvOutput{hash_it->second, output_name};
                  auto realisation = store.query_realisation(drv_output);
                  if (realisation) {
                    log_debug("firecracker: found realisation for CA floating output %s:%s -> %s",
                              store.printStorePath(input_drv_path), output_name,
                              store.printStorePath(realisation->out_path));
                    return realisation->out_path;
                  }
                }
                // No realisation found - this derivation needs to be built first
                log_debug("firecracker: no realisation for CA floating output %s:%s",
                          store.printStorePath(input_drv_path), output_name);
                return std::nullopt;
              } else if constexpr (std::is_same_v<T, ::nix::derivation_output_t::Deferred>) {
                // Deferred outputs - similar to floating, check realisations
                auto drv_hashes = ::nix::static_output_hashes(store, input_drv);
                auto hash_it = drv_hashes.find(output_name);
                if (hash_it != drv_hashes.end()) {
                  auto drv_output = ::nix::DrvOutput{hash_it->second, output_name};
                  auto realisation = store.query_realisation(drv_output);
                  if (realisation) {
                    return realisation->out_path;
                  }
                }
                return std::nullopt;
              } else {
                // Impure or unknown - cannot substitute
                return std::nullopt;
              }
            },
            output_it->second.raw);

        if (output_path_opt) {
          // Try to substitute this output if it's not already available
          if (!store.isValidPath(*output_path_opt)) {
            log_info("firecracker: need input %s (output '%s' of %s)",
                     store.printStorePath(*output_path_opt), output_name,
                     store.printStorePath(input_drv_path));
            ensure_path(store, *output_path_opt);
          }
        } else {
          // No output path determined - either CA floating without realisation, or impure
          // The caller will need to build this derivation first
          log_warn("firecracker: cannot determine output path for %s:%s - may need to build",
                   store.printStorePath(input_drv_path), output_name);
        }
      }

      // Recursively handle nested inputs (childMap)
      // TODO: handle nested derivation inputs if needed
    }
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
    // Check for /dev/kvm (required for KVM virtualization)
    if (access("/dev/kvm", R_OK | W_OK) != 0) {
      log_debug("firecracker: /dev/kvm not accessible");
      return false;
    }

    // If embedded guest data is available, we don't need external kernel/initrd
    if (embedded_guest::is_available()) {
      log_debug("firecracker: using embedded kernel/initrd");
      return true;
    }

    // Check for kernel (required for VM boot)
    if (config_.kernel_path.empty() || !fs::exists(config_.kernel_path)) {
      log_debug("firecracker: kernel not found at %s", config_.kernel_path);
      return false;
    }

    // Check for initrd (required for guest init)
    if (config_.initrd_path.empty() || !fs::exists(config_.initrd_path)) {
      log_debug("firecracker: initrd not found at %s", config_.initrd_path);
      return false;
    }

    // Note: firecracker binary no longer required - VMM is embedded via vmm-ffi
    return true;
  }

  auto resolve_derived_path_built(::nix::store_t& store, const ::nix::derived_path_t::Built& built)
      -> std::optional<::nix::store_path_t> {
    return std::visit(
        [&](const auto& d) -> std::optional<::nix::store_path_t> {
          using T = std::decay_t<decltype(d)>;
          if constexpr (std::is_same_v<T, ::nix::derived_path_t::opaque_t>) {
            return d.path;
          } else {
            // Recursively resolve
            return std::nullopt;
          }
        },
        built.drv_path->raw());
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

    // Prepare the VM images (store.ext4, output.ext4)
    prepare_vm_config(store, drv, work_dir, inputs);

    // Start the VM (embedded VMM - no fork/exec)
    auto vm = start_vm(work_dir, inputs);
    if (!vm || !vm->running) {
      result.inner =
          ::nix::build_result_t::Failure{.status = ::nix::build_result_t::Failure::MiscFailure,
                                         .errorMsg = "Failed to start Firecracker VM"};
      return result;
    }

    // Execute the builder inside the VM
    result = run_builder_in_vm(*vm, store, drv, witness);

    // Extract outputs if build succeeded
    if (result.tryGetSuccess()) {
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

    // Always use a unique directory per build to support concurrent builds of
    // the same derivation. Using just the derivation name caused race conditions
    // where multiple builds would corrupt each other's work directories.
    auto name = std::string(drv_path.name());
    static std::atomic<uint64_t> counter{0};
    auto unique_id = std::to_string(getpid()) + "." + std::to_string(counter++);
    auto work = base / (name + "." + unique_id);

    // Clean up any stale directory with the same name (shouldn't happen with unique IDs)
    if (fs::exists(work)) {
      // Unmount any stale FUSE mounts before cleanup
      auto store_mount = work / "store-mount";
      auto output_mount = work / "output-mount";
      if (fs::exists(store_mount)) {
        auto cmd = "fusermount -u " + store_mount.string() + " 2>/dev/null";
        system(cmd.c_str());
      }
      if (fs::exists(output_mount)) {
        auto cmd = "fusermount -u " + output_mount.string() + " 2>/dev/null";
        system(cmd.c_str());
      }
      std::error_code ec;
      fs::remove_all(work, ec);
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

    log_debug("firecracker: collecting inputs, input_srcs has %zu entries", drv.input_srcs.size());

    // Collect all direct inputs (input_srcs + builder)
    ::nix::store_path_set_t direct_inputs;
    for (const auto& src : drv.input_srcs) {
      direct_inputs.insert(src);
      log_debug("firecracker: input_src: %s", store.printStorePath(src));
    }

    // Add builder if it's a store path
    if (drv.builder.starts_with("/nix/store")) {
      log_debug("firecracker: builder: %s", drv.builder);
      // builder may be /nix/store/<hash>-<name>/bin/foo, extract base store path
      auto [builder_path, _suffix] = store.toStorePath(drv.builder);
      direct_inputs.insert(builder_path);
    }

    // Compute the closure of all inputs (includes all runtime dependencies)
    ::nix::store_path_set_t closure;
    store.computeFSClosure(direct_inputs, closure);

    log_info("firecracker: closure has %zu paths (from %zu direct inputs)", closure.size(),
             direct_inputs.size());

    // Ensure all paths in the closure are available (substitute if needed)
    for (const auto& path : closure) {
      ensure_path(store, path);
    }

    // Categorize each path in the closure
    for (const auto& path : closure) {
      auto path_str = store.printStorePath(path);
      log_info("firecracker: closure path: %s", path_str);
      auto real_path = resolve_store_path(path_str);
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

    // Generate Firecracker VM config JSON using std::format
    auto config_json = std::format(
        R"({{
  "boot-source": {{
    "kernel_image_path": "{}",
    "initrd_path": "{}",
    "boot_args": "console=ttyS0 reboot=k panic=1 pci=off init=/init"
  }},
  "machine-config": {{
    "vcpu_count": {},
    "mem_size_mib": {}
  }},
  "drives": [
    {{
      "drive_id": "store",
      "path_on_host": "{}/store.ext4",
      "is_root_device": false,
      "is_read_only": true
    }},
    {{
      "drive_id": "output",
      "path_on_host": "{}/output.ext4",
      "is_root_device": false,
      "is_read_only": false
    }}
  ],
  "vsock": {{
    "guest_cid": 3,
    "uds_path": "{}/vsock.sock"
  }}
}}
)",
        config_.kernel_path, config_.initrd_path, config_.vcpu_count, config_.mem_size_mib,
        work_dir, work_dir, work_dir);

    // Write config to file
    auto config_path = work_dir + "/vm-config.json";
    std::ofstream config_file(config_path);
    config_file << config_json;
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
    // fakeroot allows creating files as if we were root (they'll have root ownership in the image)
    auto fuse_cmd = "fuse2fs -o rw,fakeroot " + image_path + " " + mount_point;

    // Capture fuse2fs stderr for debugging mount failures
    std::string fuse_error;
    auto fuse_cmd_capture = fuse_cmd + " 2>&1";
    FILE* pipe = popen(fuse_cmd_capture.c_str(), "r");
    if (pipe) {
      char buffer[256];
      while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        fuse_error += buffer;
      }
      int status = pclose(pipe);
      if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        mounted = true;
        log_debug("firecracker: mounted store image via fuse2fs");
      } else {
        log_debug("firecracker: fuse2fs failed with status %d: %s",
                  WIFEXITED(status) ? WEXITSTATUS(status) : -1, fuse_error);
      }
    }

    if (!mounted) {
      // Fallback to loop mount (requires privileges)
      auto loop_cmd = "mount -o loop " + image_path + " " + mount_point + " 2>&1";
      std::string loop_error;
      pipe = popen(loop_cmd.c_str(), "r");
      if (pipe) {
        char buffer[256];
        while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
          loop_error += buffer;
        }
        int status = pclose(pipe);
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
          mounted = true;
          log_debug("firecracker: mounted store image via loop");
        } else {
          log_debug("firecracker: loop mount failed: %s", loop_error);
        }
      }
    }

    if (!mounted) {
      throw ::nix::Error("failed to mount store image - fuse2fs error: " + fuse_error);
    }

    // Create /store directory structure inside the image
    // The guest mounts this image at /nix, so paths end up at /nix/store
    auto store_dir = fs::path(mount_point) / "store";
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
        log_warn("firecracker: failed to copy %s: %s", src_path, e.what());
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
      log_warn("firecracker: failed to unmount store image cleanly");
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

    // Format as ext4 (-L "" removes lost+found which causes permission issues with fuse)
    auto cmd = "mkfs.ext4 -q -F -L '' -m 0 " + path;
    if (system(cmd.c_str()) != 0) {
      throw ::nix::Error("failed to format image as ext4: " + path);
    }

    // Remove lost+found directory by mounting briefly
    // fuse2fs creates it on mount, we don't need it and it causes cleanup issues
    auto tmp_mount = path + ".tmp_mount";
    fs::create_directories(tmp_mount);
    auto mount_cmd = "fuse2fs -o rw,fakeroot " + path + " " + tmp_mount;
    log_debug("firecracker: create_ext4_image mounting %s at %s", path, tmp_mount);
    if (system(mount_cmd.c_str()) == 0) {
      auto lf_path = tmp_mount + "/lost+found";
      if (fs::exists(lf_path)) {
        fs::remove_all(lf_path);
      }
      auto umount_cmd = "fusermount -u " + tmp_mount;
      int umount_status = system(umount_cmd.c_str());
      if (umount_status != 0) {
        log_debug("firecracker: create_ext4_image unmount failed with %d, trying fusermount3",
                  umount_status);
        umount_cmd = "fusermount3 -u " + tmp_mount;
        umount_status = system(umount_cmd.c_str());
      }
      // Ensure unmount completed before returning - wait for mountpoint to become available
      if (umount_status == 0) {
        // Give kernel time to release the mount
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
      log_debug("firecracker: create_ext4_image unmount status: %d", umount_status);
    } else {
      log_debug("firecracker: create_ext4_image mount failed for lost+found cleanup");
    }
    fs::remove_all(tmp_mount);
  }

  auto start_vm(const std::string& work_dir, const categorized_inputs& /*inputs*/)
      -> std::unique_ptr<vm_instance> {
    auto vm = std::make_unique<vm_instance>();
    vm->work_dir = work_dir;
    vm->vsock_uds_path = work_dir + "/vsock.sock";

    // Build kernel command line
    // pci=off: disable PCI enumeration (not needed for virtio-mmio)
    // acpi=off: disable ACPI to prevent PCI memory region reservation that conflicts with
    // virtio-mmio
    std::string kernel_cmdline = "console=ttyS0 reboot=k panic=1 pci=off acpi=off init=/init";

    int error = VMM_OK;

    // Try to use embedded guest data if available
    if (embedded_guest::is_available()) {
      log_debug("firecracker: using embedded kernel/initrd");

      auto kernel = embedded_guest::kernel_data();
      auto initrd = embedded_guest::initrd_data();

      VmmEmbeddedConfig embedded_config{};
      embedded_config.kernel_data = kernel.data();
      embedded_config.kernel_size = kernel.size();
      embedded_config.initrd_data = initrd.data();
      embedded_config.initrd_size = initrd.size();
      embedded_config.kernel_cmdline = kernel_cmdline.c_str();
      embedded_config.vcpu_count = config_.vcpu_count;
      embedded_config.mem_size_mib = config_.mem_size_mib;
      embedded_config.kernel_compression = embedded_guest::compression_format();
      embedded_config.initrd_compression = VMM_COMPRESS_NONE; // initrd is already gzip for kernel

      vm->vmm_handle = vmm_create_from_embedded(&embedded_config, &error);
    } else {
      // Fall back to file-based configuration
      log_debug("firecracker: using file-based kernel/initrd");

      VmmConfig vmm_config{};
      vmm_config.kernel_path = config_.kernel_path.c_str();
      vmm_config.initrd_path = config_.initrd_path.c_str();
      vmm_config.kernel_cmdline = kernel_cmdline.c_str();
      vmm_config.vcpu_count = config_.vcpu_count;
      vmm_config.mem_size_mib = config_.mem_size_mib;

      vm->vmm_handle = vmm_create(&vmm_config, &error);
    }

    if (!vm->vmm_handle) {
      log_error("firecracker: vmm_create failed: %s", vmm_strerror(error));
      return nullptr;
    }

    // Add store block device (read-only)
    std::string store_image = work_dir + "/store.ext4";
    std::string store_id = "store";
    VmmBlockDevice store_device{};
    store_device.drive_id = store_id.c_str();
    store_device.path = store_image.c_str();
    store_device.is_read_only = 1;

    error = vmm_add_block_device(vm->vmm_handle, &store_device);
    if (error != VMM_OK) {
      log_error("firecracker: vmm_add_block_device(store) failed: %s", vmm_strerror(error));
      vmm_destroy(vm->vmm_handle);
      vm->vmm_handle = nullptr;
      return nullptr;
    }

    // Add output block device (read-write)
    std::string output_image = work_dir + "/output.ext4";
    std::string output_id = "output";
    VmmBlockDevice output_device{};
    output_device.drive_id = output_id.c_str();
    output_device.path = output_image.c_str();
    output_device.is_read_only = 0;

    error = vmm_add_block_device(vm->vmm_handle, &output_device);
    if (error != VMM_OK) {
      log_error("firecracker: vmm_add_block_device(output) failed: %s", vmm_strerror(error));
      vmm_destroy(vm->vmm_handle);
      vm->vmm_handle = nullptr;
      return nullptr;
    }

    // Configure vsock
    VmmVsockConfig vsock_config{};
    vsock_config.guest_cid = 3;
    vsock_config.uds_path = vm->vsock_uds_path.c_str();

    error = vmm_configure_vsock(vm->vmm_handle, &vsock_config);
    if (error != VMM_OK) {
      log_error("firecracker: vmm_configure_vsock failed: %s", vmm_strerror(error));
      vmm_destroy(vm->vmm_handle);
      vm->vmm_handle = nullptr;
      return nullptr;
    }

    // Start the VM
    error = vmm_start(vm->vmm_handle);
    if (error != VMM_OK) {
      log_error("firecracker: vmm_start failed: %s", vmm_strerror(error));
      vmm_destroy(vm->vmm_handle);
      vm->vmm_handle = nullptr;
      return nullptr;
    }

    // Start the event loop in a background thread.
    // This is CRITICAL - without the event loop running, virtio devices
    // won't function because MMIO accesses and interrupts aren't processed.
    error = vmm_start_event_loop(vm->vmm_handle);
    if (error != VMM_OK) {
      log_error("firecracker: vmm_start_event_loop failed: %s", vmm_strerror(error));
      vmm_shutdown(vm->vmm_handle);
      vmm_destroy(vm->vmm_handle);
      vm->vmm_handle = nullptr;
      return nullptr;
    }

    vm->running = true;

    // Wait for vsock socket to appear (indicates VM is ready for connections)
    auto deadline = std::chrono::steady_clock::now() + config_.boot_timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      if (access(vm->vsock_uds_path.c_str(), F_OK) == 0) {
        // Socket exists, give it a moment to be ready
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

    log_info("firecracker: VM started (embedded), vsock=%s", vm->vsock_uds_path);
    return vm;
  }

  auto run_builder_in_vm(vm_instance& vm, ::nix::store_t& store,
                         const ::nix::basic_derivation_t& drv, build_witness& witness)
      -> ::nix::build_result_t {
    ::nix::build_result_t result;
    // Default to failure
    auto set_failure = [&](::nix::build_result_t::Failure::Status status, std::string msg) {
      result.inner = ::nix::build_result_t::Failure{.status = status, .errorMsg = std::move(msg)};
    };
    set_failure(::nix::build_result_t::Failure::MiscFailure, "");

    log_info("firecracker: executing builder %s", drv.builder);

    // Connect to guest via vsock
    vsock_client vsock;
    // Wait up to 30s for guest to boot and start listening
    // Boot is fast (~100ms) but init needs to mount filesystems and start vsock listener
    if (!vsock.connect_to_guest(vm.vsock_uds_path, VM_VSOCK_BUILD_PORT,
                                std::chrono::milliseconds(30000))) {
      set_failure(::nix::build_result_t::Failure::MiscFailure,
                  "Failed to connect to guest builder via vsock");
      return result;
    }

    // Prepare build request
    build_exec_request req;
    req.builder = drv.builder;
    req.args = std::vector<std::string>(drv.args.begin(), drv.args.end());
    req.workdir = "/build"; // Standard build directory in guest

    // Get expected output paths
    auto outputs_and_paths = drv.outputsAndOptPaths(store);
    for (const auto& [output_name, output_and_path] : outputs_and_paths) {
      const auto& [output, opt_path] = output_and_path;
      if (opt_path) {
        req.outputs.push_back(store.printStorePath(*opt_path));
      }
    }

    // Parse derivation options and create desugared environment
    // This handles passAsFile, structuredAttrs, exportReferencesGraph, etc.
    const ::nix::StructuredAttrs* structured_attrs_ptr =
        drv.structured_attrs ? &*drv.structured_attrs : nullptr;
    auto drv_options = ::nix::derivation_options_from_structured_attrs(
        store, drv.env, structured_attrs_ptr, false /* don't warn about unknown attrs */);

    // Collect input paths for DesugaredEnv::create
    ::nix::store_path_set_t input_paths;
    for (const auto& src : drv.input_srcs) {
      input_paths.insert(src);
    }

    // Create desugared environment
    auto desugared_env = ::nix::DesugaredEnv::create(store, drv, drv_options, input_paths);

    // Add standard Nix build environment variables
    // These are expected by stdenv and most builders
    req.env.emplace_back("NIX_BUILD_TOP", req.workdir);
    req.env.emplace_back("TMPDIR", req.workdir);
    req.env.emplace_back("TEMPDIR", req.workdir);
    req.env.emplace_back("TMP", req.workdir);
    req.env.emplace_back("TEMP", req.workdir);
    req.env.emplace_back("HOME", "/homeless-shelter");
    req.env.emplace_back("PATH", "/path-not-set");
    req.env.emplace_back("NIX_STORE", "/nix/store");
    req.env.emplace_back("PWD", req.workdir);

    // Convert desugared environment variables to request format
    for (const auto& [key, entry] : desugared_env.variables) {
      std::string value = entry.value;
      if (entry.prependBuildDirectory) {
        // Prepend the build directory to the value (for file references)
        value = req.workdir + "/" + entry.value;
      }
      req.env.emplace_back(key, value);
    }

    // Add extra files from desugared environment (passAsFile contents, structuredAttrs, etc.)
    for (const auto& [filename, contents] : desugared_env.extraFiles) {
      req.extra_files.emplace_back(filename, contents);
      log_debug("firecracker: adding extra file '%s' (%zu bytes)", filename.c_str(),
                contents.size());
    }

    // Send BUILD_EXEC
    auto payload = req.serialize();
    if (!vsock.send_message(vm_msg_type::BUILD_EXEC, std::span<const uint8_t>(payload))) {
      set_failure(::nix::build_result_t::Failure::MiscFailure,
                  "Failed to send build request to guest");
      return result;
    }

    log_debug("firecracker: sent BUILD_EXEC, waiting for completion...");

    // Process responses until we get BUILD_EXIT
    auto timeout = std::chrono::duration_cast<std::chrono::milliseconds>(config_.build_timeout);
    std::string build_log;

    while (true) {
      auto hdr = vsock.recv_header(timeout);
      if (!hdr) {
        set_failure(::nix::build_result_t::Failure::MiscFailure,
                    "Lost connection to guest during build");
        return result;
      }

      auto msg_payload = vsock.recv_payload(hdr->payload_len, timeout);
      if (msg_payload.empty() && hdr->payload_len > 0) {
        set_failure(::nix::build_result_t::Failure::MiscFailure,
                    "Failed to read message payload from guest");
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
            result.inner =
                ::nix::build_result_t::Success{.status = ::nix::build_result_t::Success::Built};
            log_info("firecracker: build succeeded");
          } else {
            auto error_msg = exit_resp.error_msg.empty()
                                 ? "Builder exited with code " + std::to_string(exit_resp.exit_code)
                                 : exit_resp.error_msg;
            set_failure(::nix::build_result_t::Failure::PermanentFailure, error_msg);
            log_error("firecracker: build failed: %s", error_msg.c_str());
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
              log_warn("firecracker: build attempted network access: %s", evt.path);
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
          log_warn("firecracker: unexpected message type 0x%04x", static_cast<int>(msg_type));
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
    // 3. For CA floating outputs, hash content and compute final path
    // 4. Register in store database (and realisations for CA)

    auto output_image = work_dir + "/output.ext4";
    auto mount_point = work_dir + "/output-mount";

    // Create mount point
    fs::create_directories(mount_point);

    // Try to mount using fuse2fs (ext4 FUSE driver) - no root required
    // Note: ext4 images may need rw mount first if journal is dirty
    auto mount_cmd = "fuse2fs " + output_image + " " + mount_point + " 2>/dev/null";
    bool mounted = (system(mount_cmd.c_str()) == 0);

    if (!mounted) {
      // Fallback: try loop mount (requires privileges)
      // Use rw mount - ext4 from guest may have dirty journal that prevents ro mount
      mount_cmd = "mount -o loop " + output_image + " " + mount_point + " 2>/dev/null";
      mounted = (system(mount_cmd.c_str()) == 0);
    }

    if (!mounted) {
      log_error("firecracker: failed to mount output image at %s", mount_point);
      log_error("firecracker: outputs may need manual extraction from %s", output_image);
      return;
    }

    log_info("firecracker: mounted output image at %s", mount_point);

    // Compute output hashes for CA derivation support
    // We need this to create DrvOutput keys for realisation registration
    std::map<std::string, ::nix::Hash> output_hashes;
    try {
      // Cast to derivation_t to access static_output_hashes
      // This is safe because basic_derivation_t is a base of derivation_t
      // and we're only using it for hash computation
      output_hashes =
          ::nix::static_output_hashes(store, static_cast<const ::nix::derivation_t&>(drv));
    } catch (...) {
      // If this fails (e.g., drv is truly just a basic_derivation_t), continue without CA support
      log_debug("firecracker: could not compute output hashes (non-CA derivation)");
    }

    // Extract each output
    auto outputs_and_paths = drv.outputsAndOptPaths(store);
    for (const auto& [name, output_and_path] : outputs_and_paths) {
      const auto& [output, opt_path] = output_and_path;

      // For CA floating outputs, we need to find the output in the VM's store
      // by scanning for outputs matching the derivation name pattern
      fs::path src_path;
      std::string basename;

      if (opt_path) {
        // Input-addressed or CA fixed: path is known
        auto output_path = store.printStorePath(*opt_path);
        basename = fs::path(output_path).filename().string();
        src_path = fs::path(mount_point) / "nix" / "store" / basename;
      } else {
        // CA floating or deferred: need to find the output by name pattern
        // The builder writes to $out which the guest sets up based on drv.env
        // Look for paths matching the expected output name
        auto store_dir = fs::path(mount_point) / "nix" / "store";
        if (fs::exists(store_dir)) {
          for (const auto& entry : fs::directory_iterator(store_dir)) {
            auto entry_name = entry.path().filename().string();
            // Match by derivation name suffix (e.g., "*-foo" for output "out" of drv "foo")
            // For non-default outputs, look for "*-foo-<output_name>"
            std::string suffix = "-" + drv.name;
            if (name != "out") {
              suffix += "-" + name;
            }
            if (entry_name.size() > suffix.size() &&
                entry_name.substr(entry_name.size() - suffix.size()) == suffix) {
              src_path = entry.path();
              basename = entry_name;
              log_info("firecracker: found CA floating output '%s' at %s", name, src_path.c_str());
              break;
            }
          }
        }

        if (src_path.empty()) {
          log_warn("firecracker: could not find output '%s' (floating/deferred)", name);
          continue;
        }
      }

      if (!fs::exists(src_path)) {
        log_error("firecracker: output '%s' not found at %s", name, src_path.c_str());
        continue;
      }

      // Destination: prefer user store (writable), fallback to system store
      fs::path dst_path;
      if (!config_.user_store_dir.empty()) {
        fs::create_directories(config_.user_store_dir);
        dst_path = fs::path(config_.user_store_dir) / basename;
      } else {
        dst_path = fs::path("/nix/store") / basename;
      }

      log_info("firecracker: extracting output '%s': %s -> %s", name, src_path.c_str(),
               dst_path.c_str());

      try {
        // Remove existing if present (shouldn't happen with CA, but safety first)
        if (fs::exists(dst_path)) {
          fs::remove_all(dst_path);
        }

        // Copy the output
        fs::copy(src_path, dst_path, fs::copy_options::recursive);

        // Register the realisation for CA derivations
        auto hash_it = output_hashes.find(name);
        if (hash_it != output_hashes.end() &&
            ::nix::experimental_feature_settings.is_enabled(::nix::xp_t::ca_derivations)) {
          // Parse the output path from basename
          auto out_path = store.parseStorePath("/nix/store/" + basename);

          // Create and register the realisation
          // realisation_t = { UnkeyedRealisation{out_path, sigs, deps}, id }
          ::nix::realisation_t realisation{
              {
                  .out_path = out_path,
                  .signatures = {},
                  .dependentRealisations = {},
              },
              ::nix::DrvOutput{hash_it->second, name},
          };

          try {
            store.register_drv_output(realisation);
            log_info("firecracker: registered realisation for %s:%s -> %s", drv.name, name,
                     store.printStorePath(out_path));
          } catch (const std::exception& e) {
            log_warn("firecracker: could not register realisation: %s", e.what());
          }
        }

        witness.outputs_written.push_back(dst_path.string());
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

  // Note: firecracker binary no longer needed - VMM is embedded via vmm-ffi

  // Check environment variables for kernel/initrd paths
  const char* fc_kernel = getenv("NIX_FIRECRACKER_KERNEL");
  const char* fc_initrd = getenv("NIX_FIRECRACKER_INITRD");

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

  // Log configuration (note: VMM is embedded, no separate binary needed)
  log_debug("firecracker: using embedded VMM (vmm-ffi)");
  if (!config.kernel_path.empty()) {
    log_debug("firecracker: kernel = %s", config.kernel_path);
  }
  if (!config.initrd_path.empty()) {
    log_debug("firecracker: initrd = %s", config.initrd_path);
  }

  return std::make_unique<firecracker_build_service>(std::move(config));
}

std::unique_ptr<build_service> make_firecracker_build_service(const std::string& kernel_path,
                                                              const std::string& initrd_path) {
  firecracker_config config;
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
