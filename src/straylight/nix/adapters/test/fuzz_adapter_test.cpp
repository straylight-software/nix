// straylight // nix // adapters // test
//
// Fuzz tests for the store adapter bridging straylight to nix.
//
// Key attack surfaces:
// - NAR parsing and restoration
// - Path info conversion between formats
// - Signature verification
// - Store path parsing and validation

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

// Nix headers must come before property.h since they have their own macros
#include "straylight/nix/adapters/store_adapter.h"

#include "nix/main/shared.h"
#include "nix/store/path-info.h"
#include "nix/store/path.h"
#include "nix/util/archive.h"
#include "nix/util/hash.h"
#include "nix/util/serialise.h"

// Property testing - MUST come last to get Catch2 macros
#include "nix/tests/property.h"

namespace fs = std::filesystem;

// Initialize nix library once at startup
struct NixInitializer {
  NixInitializer() {
    ::nix::init_nix(false); // false = don't load config
  }
};
static NixInitializer nix_init;

// =============================================================================
// Test fixture: temporary store directory
// =============================================================================

namespace {

struct temp_store {
  temp_store() {
    auto pid = static_cast<std::uint64_t>(getpid());
    auto tid = std::hash<std::thread::id>{}(std::this_thread::get_id());
    auto cnt = counter_++;
    auto rnd =
        static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());

    path_ = fs::temp_directory_path() /
            ("fuzz_adapter_" + std::to_string(pid) + "_" + std::to_string(tid) + "_" +
             std::to_string(cnt) + "_" + std::to_string(rnd % 1000000));
    fs::create_directories(path_);
    fs::create_directories(path_ / "store");
    fs::create_directories(path_ / "var" / "nix" / "db");
  }

  ~temp_store() {
    std::error_code ec;
    fs::remove_all(path_, ec);
  }

  auto path() const -> const fs::path& { return path_; }

private:
  fs::path path_;
  static inline std::atomic<std::uint64_t> counter_{0};
};

// Generate valid-looking store path strings
auto gen_store_path_string() {
  return rc::gen::apply(
      [](const std::string& hash_input, const std::string& name_input) {
        // Generate 32-char nix32 hash
        std::string hash;
        const char* nix32 = "0123456789abcdfghijklmnpqrsvwxyz";
        for (std::size_t i = 0; i < 32; ++i) {
          char c = hash_input.empty() ? 'a' : hash_input[i % hash_input.size()];
          // Map to nix32 alphabet
          hash += nix32[static_cast<unsigned char>(c) % 32];
        }
        // Sanitize name
        std::string name;
        for (char c : name_input) {
          if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '-' || c == '_' || c == '.') {
            name += c;
          }
        }
        if (name.empty()) {
          name = "pkg";
        }
        return "/nix/store/" + hash + "-" + name;
      },
      rc::gen::arbitrary<std::string>(), rc::gen::arbitrary<std::string>());
}

// Generate valid hex hash for SHA256
auto gen_sha256_hex() {
  return rc::gen::apply(
      [](const std::string& input) {
        std::string hash;
        hash.reserve(64);
        for (std::size_t i = 0; i < 64; ++i) {
          char c = input.empty() ? '0' : input[i % input.size()];
          if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) {
            hash += c;
          } else if (c >= 'A' && c <= 'F') {
            hash += static_cast<char>(c - 'A' + 'a');
          } else {
            hash += '0';
          }
        }
        return hash;
      },
      rc::gen::arbitrary<std::string>());
}

} // namespace

// =============================================================================
// Store path parsing fuzz tests
// =============================================================================

TEST_CASE("fuzz: parseStorePath handles arbitrary strings", "[fuzz][adapter][path]") {
  rc::prop("parseStorePath never crashes", []() {
    temp_store tmp;
    ::nix::store_config_t::Params params;
    params["store"] = "/nix/store";
    params["real"] = (tmp.path() / "store").string();
    params["state"] = (tmp.path() / "var" / "nix").string();
    params["log"] = (tmp.path() / "var" / "log" / "nix").string();

    auto config =
        ::nix::make_ref<straylight::nix::adapters::StoreAdapterConfig>("straylight", "", params);
    auto store = ::nix::make_ref<straylight::nix::adapters::store_adapter>(config);

    auto input = *rc::gen::arbitrary<std::string>();
    try {
      [[maybe_unused]] auto path = store->parseStorePath(input);
    } catch (const ::nix::base_error_t&) {
      // Expected for invalid paths
    } catch (const std::exception&) {
      // Other exceptions are fine too
    }
    RC_SUCCEED("No crash");
  });
}

TEST_CASE("fuzz: isValidPath handles arbitrary strings", "[fuzz][adapter][path]") {
  rc::prop("isValidPath never crashes", []() {
    temp_store tmp;
    ::nix::store_config_t::Params params;
    params["store"] = "/nix/store";
    params["real"] = (tmp.path() / "store").string();
    params["state"] = (tmp.path() / "var" / "nix").string();
    params["log"] = (tmp.path() / "var" / "log" / "nix").string();

    auto config =
        ::nix::make_ref<straylight::nix::adapters::StoreAdapterConfig>("straylight", "", params);
    auto store = ::nix::make_ref<straylight::nix::adapters::store_adapter>(config);

    auto path_str = *gen_store_path_string();
    try {
      auto path = store->parseStorePath(path_str);
      [[maybe_unused]] auto valid = store->isValidPath(path);
    } catch (const ::nix::base_error_t&) {
      // Expected for invalid paths
    }
    RC_SUCCEED("No crash");
  });
}

// =============================================================================
// NAR parsing fuzz tests
// =============================================================================

TEST_CASE("fuzz: NAR parsing handles malformed input", "[fuzz][adapter][nar]") {
  rc::prop("restore_path handles arbitrary NAR data", []() {
    temp_store tmp;
    auto target_path = tmp.path() / "output";

    auto nar_data = *rc::gen::arbitrary<std::string>();

    try {
      ::nix::string_source_t source(nar_data);
      ::nix::restore_path(target_path.string(), source);
    } catch (const ::nix::base_error_t&) {
      // Expected for invalid NAR
    } catch (const std::exception&) {
      // Other exceptions are fine
    }

    // Clean up any partial output
    std::error_code ec;
    fs::remove_all(target_path, ec);

    RC_SUCCEED("No crash");
  });
}

TEST_CASE("fuzz: NAR parsing with valid header prefix", "[fuzz][adapter][nar]") {
  rc::prop("restore_path handles NAR with valid header but bad body", []() {
    temp_store tmp;
    auto target_path = tmp.path() / "output";

    // NAR header: nix-archive-1, then (, then type, then type value
    auto body = *rc::gen::arbitrary<std::string>();
    std::string nar = "\x0d\x00\x00\x00\x00\x00\x00\x00nix-archive-1\x00\x00\x00";
    nar += body;

    try {
      ::nix::string_source_t source(nar);
      ::nix::restore_path(target_path.string(), source);
    } catch (const ::nix::base_error_t&) {
      // Expected for invalid NAR
    } catch (const std::exception&) {
      // Other exceptions are fine
    }

    std::error_code ec;
    fs::remove_all(target_path, ec);

    RC_SUCCEED("No crash");
  });
}

// =============================================================================
// Path info conversion fuzz tests
// =============================================================================

TEST_CASE("fuzz: valid_path_info_t handles arbitrary data", "[fuzz][adapter][pathinfo]") {
  rc::prop("path info construction doesn't crash", []() {
    temp_store tmp;
    ::nix::store_dir_config_t store_config{"/nix/store"};

    auto path_str = *gen_store_path_string();
    auto nar_hash_hex = *gen_sha256_hex();

    try {
      auto path = store_config.parseStorePath(path_str);
      auto nar_hash =
          ::nix::hash_t::parse_any(nar_hash_hex, std::optional(::nix::hash_algorithm_t::SHA256));

      ::nix::valid_path_info_t info(
          path, ::nix::UnkeyedValidPathInfo{store_config.store_dir, std::move(nar_hash)});

      info.nar_size = std::abs(*rc::gen::arbitrary<std::int64_t>() % 1000000000);
      info.registrationTime = std::abs(*rc::gen::arbitrary<std::int64_t>() % 2000000000);
      info.ultimate = *rc::gen::arbitrary<bool>();

      // Add arbitrary signatures
      auto sig_count = *rc::gen::inRange(0, 5);
      for (int i = 0; i < sig_count; ++i) {
        info.sigs.insert(*rc::gen::arbitrary<std::string>());
      }

    } catch (const ::nix::base_error_t&) {
      // Expected for invalid data
    } catch (const std::exception&) {
      // Other exceptions are fine
    }
    RC_SUCCEED("No crash");
  });
}

// =============================================================================
// Signature verification fuzz tests
// =============================================================================

TEST_CASE("fuzz: signature verification handles malformed signatures", "[fuzz][adapter][sig]") {
  rc::prop("pathInfoIsUntrusted handles arbitrary signatures", []() {
    temp_store tmp;
    ::nix::store_config_t::Params params;
    params["store"] = "/nix/store";
    params["real"] = (tmp.path() / "store").string();
    params["state"] = (tmp.path() / "var" / "nix").string();
    params["log"] = (tmp.path() / "var" / "log" / "nix").string();

    auto config =
        ::nix::make_ref<straylight::nix::adapters::StoreAdapterConfig>("straylight", "", params);
    auto store = ::nix::make_ref<straylight::nix::adapters::store_adapter>(config);

    auto path_str = *gen_store_path_string();
    auto nar_hash_hex = *gen_sha256_hex();

    try {
      auto path = store->parseStorePath(path_str);
      auto nar_hash =
          ::nix::hash_t::parse_any(nar_hash_hex, std::optional(::nix::hash_algorithm_t::SHA256));

      ::nix::valid_path_info_t info(
          path, ::nix::UnkeyedValidPathInfo{store->store_dir, std::move(nar_hash)});

      info.nar_size = 1;

      // Add arbitrary malformed signatures
      auto sig_count = *rc::gen::inRange(0, 10);
      for (int i = 0; i < sig_count; ++i) {
        info.sigs.insert(*rc::gen::arbitrary<std::string>());
      }

      // Should not crash, just return true (untrusted) for bad signatures
      [[maybe_unused]] auto untrusted = store->pathInfoIsUntrusted(info);

    } catch (const ::nix::base_error_t&) {
      // Expected for invalid data
    } catch (const std::exception&) {
      // Other exceptions are fine
    }
    RC_SUCCEED("No crash");
  });
}

// =============================================================================
// Store operations fuzz tests
// =============================================================================

TEST_CASE("fuzz: query_path_info handles arbitrary paths", "[fuzz][adapter][query]") {
  rc::prop("query_path_info never crashes", []() {
    temp_store tmp;
    ::nix::store_config_t::Params params;
    params["store"] = "/nix/store";
    params["real"] = (tmp.path() / "store").string();
    params["state"] = (tmp.path() / "var" / "nix").string();
    params["log"] = (tmp.path() / "var" / "log" / "nix").string();

    auto config =
        ::nix::make_ref<straylight::nix::adapters::StoreAdapterConfig>("straylight", "", params);
    auto store = ::nix::make_ref<straylight::nix::adapters::store_adapter>(config);

    auto path_str = *gen_store_path_string();

    try {
      auto path = store->parseStorePath(path_str);

      // Use callback-based API
      std::shared_ptr<const ::nix::valid_path_info_t> result;
      std::exception_ptr exc;

      store->queryPathInfo(path, {[&](std::future<::nix::ref<const ::nix::valid_path_info_t>> fut) {
                             try {
                               result = fut.get().get_ptr();
                             } catch (...) {
                               exc = std::current_exception();
                             }
                           }});

      // Result might be null or have exception - both are fine

    } catch (const ::nix::base_error_t&) {
      // Expected for invalid paths
    } catch (const std::exception&) {
      // Other exceptions are fine
    }
    RC_SUCCEED("No crash");
  });
}

// =============================================================================
// Edge cases
// =============================================================================

TEST_CASE("fuzz: adapter handles path traversal attempts", "[fuzz][adapter][security]") {
  temp_store tmp;
  ::nix::store_config_t::Params params;
  params["store"] = "/nix/store";
  params["real"] = (tmp.path() / "store").string();
  params["state"] = (tmp.path() / "var" / "nix").string();
  params["log"] = (tmp.path() / "var" / "log" / "nix").string();

  auto config =
      ::nix::make_ref<straylight::nix::adapters::StoreAdapterConfig>("straylight", "", params);
  auto store = ::nix::make_ref<straylight::nix::adapters::store_adapter>(config);

  std::vector<std::string> attacks = {
      "/nix/store/../../../etc/passwd",
      "/nix/store/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-../../../etc/passwd",
      "/nix/store/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-foo/../../../etc/passwd",
      std::string("/nix/store/") + std::string(32, 'a') + "-" + std::string(1000, '.'),
      std::string("/nix/store/") + std::string(32, 'a') + "-" + std::string(1000, '/'),
  };

  for (const auto& attack : attacks) {
    try {
      auto path = store->parseStorePath(attack);
      // If it parses, check that the real path stays within the store
      auto real = store->toRealPath(path);
      CHECK(real.find("..") == std::string::npos);
    } catch (const ::nix::base_error_t&) {
      // Expected - attack should be rejected
    }
  }
}

TEST_CASE("fuzz: adapter handles empty and null inputs", "[fuzz][adapter][edge]") {
  temp_store tmp;
  ::nix::store_config_t::Params params;
  params["store"] = "/nix/store";
  params["real"] = (tmp.path() / "store").string();
  params["state"] = (tmp.path() / "var" / "nix").string();
  params["log"] = (tmp.path() / "var" / "log" / "nix").string();

  auto config =
      ::nix::make_ref<straylight::nix::adapters::StoreAdapterConfig>("straylight", "", params);
  auto store = ::nix::make_ref<straylight::nix::adapters::store_adapter>(config);

  // Empty path
  CHECK_THROWS(store->parseStorePath(""));

  // Path with null bytes
  std::string null_path = "/nix/store/";
  null_path += std::string(32, 'a');
  null_path += "-test";
  null_path += '\0';
  null_path += "evil";
  CHECK_THROWS(store->parseStorePath(null_path));
}
