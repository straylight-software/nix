// straylight // nix // store // tests
//
// Protocol serialization benchmarks - critical path for daemon communication
//
// These benchmarks measure the performance of worker protocol serialization,
// which occurs on every daemon communication. This is critical for multi-user
// Nix performance where all operations go through the daemon.
//
// Benchmark scenarios:
//   1. StorePath serialization (every operation)
//   2. ValidPathInfo serialization (queryPathInfo, registerValidPaths)
//   3. BuildResult serialization (buildDerivation, buildPaths)
//   4. Large path set serialization (queryValidPaths, closures)
//   5. Derivation serialization over protocol (buildDerivation)

#include <chrono>
#include <cstring>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>

#include "nix/store/build-result.h"
#include "nix/store/content-address.h"
#include "nix/store/derivations.h"
#include "nix/store/path-info.h"
#include "nix/store/path.h"
#include "nix/store/store-dir-config.h"
#include "nix/store/worker-protocol-impl.h"
#include "nix/store/worker-protocol.h"
#include "nix/util/hash.h"
#include "nix/util/serialise.h"

namespace {

// =============================================================================
// Test fixtures and helpers
// =============================================================================

// Static store directory for lifetime management
static const std::string kStoreDir = "/nix/store";

// Get a store_dir_config_t for benchmarking
nix::store_dir_config_t get_store_config() {
  return nix::store_dir_config_t{kStoreDir};
}

// Generate a deterministic store path hash (32 base-32 chars)
std::string make_hash_part(int idx) {
  // Use a simple pattern that produces valid base-32 chars
  static constexpr std::string_view base32 = "0123456789abcdfghijklmnpqrsvwxyz";
  std::string result(32, '0');
  int val = idx;
  for (int i = 31; i >= 0 && val > 0; --i) {
    result[i] = base32[static_cast<size_t>(val % 32)];
    val /= 32;
  }
  return result;
}

// Generate a valid store path
nix::store_path_t make_store_path(int idx, const std::string& name = "pkg") {
  return nix::store_path_t(make_hash_part(idx) + "-" + name + "-" + std::to_string(idx));
}

// Generate a SHA256 hash
nix::Hash make_nar_hash(int idx) {
  std::string data = "test-nar-content-" + std::to_string(idx);
  return nix::hash_string(nix::hash_algorithm_t::sha256, data);
}

// Generate ValidPathInfo with references
nix::valid_path_info_t make_valid_path_info(const nix::store_dir_config_t& store, int idx,
                                            int num_refs = 5) {
  auto path = make_store_path(idx);
  auto nar_hash = make_nar_hash(idx);

  nix::UnkeyedValidPathInfo unkeyed(store.store_dir, nar_hash);
  unkeyed.registrationTime = 1700000000 + idx;
  unkeyed.nar_size = 1024 * (1 + (idx % 100));
  unkeyed.ultimate = (idx % 2) == 0;

  // Add some references
  for (int i = 0; i < num_refs; ++i) {
    unkeyed.references.insert(make_store_path(idx * 1000 + i, "dep"));
  }

  // Add a deriver for some paths
  if (idx % 3 == 0) {
    unkeyed.deriver = make_store_path(idx, "drv");
  }

  // Add some signatures
  if (idx % 2 == 0) {
    unkeyed.sigs.insert("cache.nixos.org-1:signature-" + std::to_string(idx));
  }

  return nix::valid_path_info_t(path, unkeyed);
}

// Generate BuildResult
nix::build_result_t make_build_result_success(int idx) {
  nix::build_result_t result;
  nix::build_result_t::Success success;
  success.status = nix::build_result_t::Success::Status::Built;

  result.timesBuilt = idx % 10;
  result.start_time = 1700000000 + idx;
  result.stopTime = 1700000000 + idx + 60;
  result.cpu_user = std::chrono::microseconds(1000000 * (idx % 10));
  result.cpu_system = std::chrono::microseconds(500000 * (idx % 5));
  result.inner = std::move(success);

  return result;
}

nix::build_result_t make_build_result_failure(int idx) {
  nix::build_result_t result;
  nix::build_result_t::Failure failure;
  failure.status = nix::build_result_t::Failure::Status::MiscFailure;
  failure.errorMsg = "Build failed for test package " + std::to_string(idx);
  failure.isNonDeterministic = (idx % 2 == 0);

  result.timesBuilt = 0;
  result.start_time = 1700000000 + idx;
  result.stopTime = 1700000000 + idx + 30;
  result.inner = std::move(failure);

  return result;
}

// Generate a basic derivation for protocol serialization
nix::basic_derivation_t make_basic_derivation(int idx, int num_outputs = 1, int num_inputs = 10,
                                              int num_env_vars = 20) {
  nix::basic_derivation_t drv;
  drv.name = "test-package-" + std::to_string(idx);
  drv.platform = "x86_64-linux";
  drv.builder = "/nix/store/builder-" + std::to_string(idx) + "/bin/bash";

  // Args
  drv.args = {"-e", "build.sh", "--flag", "value"};

  // Outputs
  for (int i = 0; i < num_outputs; ++i) {
    std::string output_name = (i == 0) ? "out" : "out" + std::to_string(i);
    auto out_path = make_store_path(idx * 100 + i, drv.name);
    nix::derivation_output_t::InputAddressed ia{.path = out_path};
    drv.outputs.emplace(output_name, nix::derivation_output_t{ia});
  }

  // Input sources
  for (int i = 0; i < num_inputs; ++i) {
    drv.input_srcs.insert(make_store_path(idx * 1000 + i, "input"));
  }

  // Environment variables
  for (int i = 0; i < num_env_vars; ++i) {
    drv.env["VAR_" + std::to_string(i)] =
        "value_" + std::to_string(i) + "_" + std::string(50, 'x'); // ~60 bytes each
  }

  // Common env vars
  drv.env["out"] = "/nix/store/" + make_hash_part(idx) + "-" + drv.name;
  drv.env["src"] = "/nix/store/" + make_hash_part(idx + 1) + "-source";
  drv.env["PATH"] = "/nix/store/coreutils/bin:/nix/store/gcc/bin:/nix/store/binutils/bin";

  return drv;
}

// Buffer-based sink for benchmarking
struct BenchSink : public nix::sink_t {
  std::string buffer;

  void operator()(std::string_view data) override { buffer.append(data); }

  void clear() { buffer.clear(); }
  const std::string& data() const { return buffer; }
  size_t size() const { return buffer.size(); }
};

// Buffer-based source for benchmarking
struct BenchSource : public nix::source_t {
  std::string_view data_;
  size_t pos_;

  explicit BenchSource(std::string_view data) : data_(data), pos_(0) {}

  size_t read(char* buf, size_t len) override {
    size_t available = data_.size() - pos_;
    size_t to_read = std::min(len, available);
    if (to_read == 0) {
      return 0;
    }
    std::memcpy(buf, data_.data() + pos_, to_read);
    pos_ += to_read;
    return to_read;
  }

  void reset() { pos_ = 0; }
};

} // anonymous namespace

// =============================================================================
// Benchmark: StorePath serialization
// =============================================================================

TEST_CASE("Protocol serialization: StorePath", "[store][protocol][benchmark]") {
  auto store = get_store_config();

  // Generate test paths
  std::vector<nix::store_path_t> paths;
  for (int i = 0; i < 100; ++i) {
    paths.push_back(make_store_path(i));
  }

  BenchSink sink;
  nix::WorkerProto::WriteConn write_conn{.to = sink, .version = PROTOCOL_VERSION};

  BENCHMARK("serialize single StorePath") {
    sink.clear();
    nix::WorkerProto::write(store, write_conn, paths[0]);
    return sink.size();
  };

  BENCHMARK("serialize 100 StorePaths") {
    sink.clear();
    for (const auto& path : paths) {
      nix::WorkerProto::write(store, write_conn, path);
    }
    return sink.size();
  };

  // Serialize once for deserialization benchmark
  sink.clear();
  nix::WorkerProto::write(store, write_conn, paths[0]);
  std::string serialized = sink.data();

  BENCHMARK("deserialize single StorePath") {
    BenchSource source(serialized);
    nix::WorkerProto::ReadConn read_conn{.from = source, .version = PROTOCOL_VERSION};
    return nix::WorkerProto::Serialise<nix::store_path_t>::read(store, read_conn);
  };

  // Serialize all paths for batch deserialization
  sink.clear();
  for (const auto& path : paths) {
    nix::WorkerProto::write(store, write_conn, path);
  }
  std::string all_serialized = sink.data();

  BENCHMARK("deserialize 100 StorePaths") {
    BenchSource source(all_serialized);
    nix::WorkerProto::ReadConn read_conn{.from = source, .version = PROTOCOL_VERSION};
    std::vector<nix::store_path_t> result;
    result.reserve(100);
    for (int i = 0; i < 100; ++i) {
      result.push_back(nix::WorkerProto::Serialise<nix::store_path_t>::read(store, read_conn));
    }
    return result.size();
  };
}

// =============================================================================
// Benchmark: ValidPathInfo serialization
// =============================================================================

TEST_CASE("Protocol serialization: ValidPathInfo", "[store][protocol][benchmark]") {
  auto store = get_store_config();

  // Generate test path infos with varying reference counts
  auto info_5refs = make_valid_path_info(store, 1, 5);
  auto info_20refs = make_valid_path_info(store, 2, 20);
  auto info_50refs = make_valid_path_info(store, 3, 50);

  BenchSink sink;
  nix::WorkerProto::WriteConn write_conn{.to = sink, .version = PROTOCOL_VERSION};

  BENCHMARK("serialize ValidPathInfo (5 refs)") {
    sink.clear();
    nix::WorkerProto::write(store, write_conn, info_5refs);
    return sink.size();
  };

  BENCHMARK("serialize ValidPathInfo (20 refs)") {
    sink.clear();
    nix::WorkerProto::write(store, write_conn, info_20refs);
    return sink.size();
  };

  BENCHMARK("serialize ValidPathInfo (50 refs)") {
    sink.clear();
    nix::WorkerProto::write(store, write_conn, info_50refs);
    return sink.size();
  };

  // Serialize for deserialization benchmarks
  sink.clear();
  nix::WorkerProto::write(store, write_conn, info_5refs);
  std::string serialized_5refs = sink.data();

  sink.clear();
  nix::WorkerProto::write(store, write_conn, info_50refs);
  std::string serialized_50refs = sink.data();

  BENCHMARK("deserialize ValidPathInfo (5 refs)") {
    BenchSource source(serialized_5refs);
    nix::WorkerProto::ReadConn read_conn{.from = source, .version = PROTOCOL_VERSION};
    return nix::WorkerProto::Serialise<nix::valid_path_info_t>::read(store, read_conn);
  };

  BENCHMARK("deserialize ValidPathInfo (50 refs)") {
    BenchSource source(serialized_50refs);
    nix::WorkerProto::ReadConn read_conn{.from = source, .version = PROTOCOL_VERSION};
    return nix::WorkerProto::Serialise<nix::valid_path_info_t>::read(store, read_conn);
  };
}

// =============================================================================
// Benchmark: BuildResult serialization
// =============================================================================

TEST_CASE("Protocol serialization: BuildResult", "[store][protocol][benchmark]") {
  auto store = get_store_config();

  auto success_result = make_build_result_success(1);
  auto failure_result = make_build_result_failure(1);

  BenchSink sink;
  nix::WorkerProto::WriteConn write_conn{.to = sink, .version = PROTOCOL_VERSION};

  BENCHMARK("serialize BuildResult (success)") {
    sink.clear();
    nix::WorkerProto::write(store, write_conn, success_result);
    return sink.size();
  };

  BENCHMARK("serialize BuildResult (failure)") {
    sink.clear();
    nix::WorkerProto::write(store, write_conn, failure_result);
    return sink.size();
  };

  // Serialize for deserialization benchmarks
  sink.clear();
  nix::WorkerProto::write(store, write_conn, success_result);
  std::string serialized_success = sink.data();

  sink.clear();
  nix::WorkerProto::write(store, write_conn, failure_result);
  std::string serialized_failure = sink.data();

  BENCHMARK("deserialize BuildResult (success)") {
    BenchSource source(serialized_success);
    nix::WorkerProto::ReadConn read_conn{.from = source, .version = PROTOCOL_VERSION};
    return nix::WorkerProto::Serialise<nix::build_result_t>::read(store, read_conn);
  };

  BENCHMARK("deserialize BuildResult (failure)") {
    BenchSource source(serialized_failure);
    nix::WorkerProto::ReadConn read_conn{.from = source, .version = PROTOCOL_VERSION};
    return nix::WorkerProto::Serialise<nix::build_result_t>::read(store, read_conn);
  };

  // Batch of results (simulating buildPathsWithResults)
  std::vector<nix::build_result_t> batch_results;
  for (int i = 0; i < 10; ++i) {
    batch_results.push_back(make_build_result_success(i));
  }

  BENCHMARK("serialize 10 BuildResults") {
    sink.clear();
    for (const auto& result : batch_results) {
      nix::WorkerProto::write(store, write_conn, result);
    }
    return sink.size();
  };
}

// =============================================================================
// Benchmark: Large path set serialization (closure operations)
// =============================================================================

TEST_CASE("Protocol serialization: Large path sets", "[store][protocol][benchmark]") {
  auto store = get_store_config();

  // Generate path sets of various sizes
  nix::store_path_set_t paths_100;
  nix::store_path_set_t paths_1000;
  nix::store_path_set_t paths_5000;

  for (int i = 0; i < 5000; ++i) {
    auto path = make_store_path(i);
    if (i < 100) {
      paths_100.insert(path);
    }
    if (i < 1000) {
      paths_1000.insert(path);
    }
    paths_5000.insert(path);
  }

  BenchSink sink;
  nix::WorkerProto::WriteConn write_conn{.to = sink, .version = PROTOCOL_VERSION};

  BENCHMARK("serialize StorePathSet (100 paths)") {
    sink.clear();
    nix::WorkerProto::write(store, write_conn, paths_100);
    return sink.size();
  };

  BENCHMARK("serialize StorePathSet (1000 paths)") {
    sink.clear();
    nix::WorkerProto::write(store, write_conn, paths_1000);
    return sink.size();
  };

  BENCHMARK("serialize StorePathSet (5000 paths)") {
    sink.clear();
    nix::WorkerProto::write(store, write_conn, paths_5000);
    return sink.size();
  };

  // Serialize for deserialization benchmarks
  sink.clear();
  nix::WorkerProto::write(store, write_conn, paths_100);
  std::string serialized_100 = sink.data();

  sink.clear();
  nix::WorkerProto::write(store, write_conn, paths_1000);
  std::string serialized_1000 = sink.data();

  sink.clear();
  nix::WorkerProto::write(store, write_conn, paths_5000);
  std::string serialized_5000 = sink.data();

  BENCHMARK("deserialize StorePathSet (100 paths)") {
    BenchSource source(serialized_100);
    nix::WorkerProto::ReadConn read_conn{.from = source, .version = PROTOCOL_VERSION};
    return nix::WorkerProto::Serialise<nix::store_path_set_t>::read(store, read_conn);
  };

  BENCHMARK("deserialize StorePathSet (1000 paths)") {
    BenchSource source(serialized_1000);
    nix::WorkerProto::ReadConn read_conn{.from = source, .version = PROTOCOL_VERSION};
    return nix::WorkerProto::Serialise<nix::store_path_set_t>::read(store, read_conn);
  };

  BENCHMARK("deserialize StorePathSet (5000 paths)") {
    BenchSource source(serialized_5000);
    nix::WorkerProto::ReadConn read_conn{.from = source, .version = PROTOCOL_VERSION};
    return nix::WorkerProto::Serialise<nix::store_path_set_t>::read(store, read_conn);
  };
}

// =============================================================================
// Benchmark: Derivation serialization over protocol
// =============================================================================

TEST_CASE("Protocol serialization: BasicDerivation", "[store][protocol][benchmark]") {
  auto store = get_store_config();

  // Generate derivations of varying complexity
  auto drv_small = make_basic_derivation(1, 1, 5, 10);
  auto drv_medium = make_basic_derivation(2, 3, 20, 30);
  auto drv_large = make_basic_derivation(3, 5, 50, 100);

  BenchSink sink;

  BENCHMARK("serialize BasicDerivation (small: 1 out, 5 in, 10 env)") {
    sink.clear();
    nix::write_derivation(sink, store, drv_small);
    return sink.size();
  };

  BENCHMARK("serialize BasicDerivation (medium: 3 out, 20 in, 30 env)") {
    sink.clear();
    nix::write_derivation(sink, store, drv_medium);
    return sink.size();
  };

  BENCHMARK("serialize BasicDerivation (large: 5 out, 50 in, 100 env)") {
    sink.clear();
    nix::write_derivation(sink, store, drv_large);
    return sink.size();
  };

  // Serialize for deserialization benchmarks
  sink.clear();
  nix::write_derivation(sink, store, drv_small);
  std::string serialized_small = sink.data();

  sink.clear();
  nix::write_derivation(sink, store, drv_medium);
  std::string serialized_medium = sink.data();

  sink.clear();
  nix::write_derivation(sink, store, drv_large);
  std::string serialized_large = sink.data();

  BENCHMARK("deserialize BasicDerivation (small)") {
    BenchSource source(serialized_small);
    nix::basic_derivation_t drv;
    nix::read_derivation(source, store, drv, "test-package-1");
    return drv.outputs.size();
  };

  BENCHMARK("deserialize BasicDerivation (medium)") {
    BenchSource source(serialized_medium);
    nix::basic_derivation_t drv;
    nix::read_derivation(source, store, drv, "test-package-2");
    return drv.outputs.size();
  };

  BENCHMARK("deserialize BasicDerivation (large)") {
    BenchSource source(serialized_large);
    nix::basic_derivation_t drv;
    nix::read_derivation(source, store, drv, "test-package-3");
    return drv.outputs.size();
  };
}

// =============================================================================
// Benchmark: Round-trip serialization (combined read/write)
// =============================================================================

TEST_CASE("Protocol serialization: Round-trip performance", "[store][protocol][benchmark]") {
  auto store = get_store_config();

  auto path = make_store_path(42);
  auto info = make_valid_path_info(store, 42, 20);
  auto result = make_build_result_success(42);
  auto drv = make_basic_derivation(42, 2, 15, 25);

  BenchSink sink;
  nix::WorkerProto::WriteConn write_conn{.to = sink, .version = PROTOCOL_VERSION};

  BENCHMARK("round-trip StorePath") {
    sink.clear();
    nix::WorkerProto::write(store, write_conn, path);
    BenchSource source(sink.data());
    nix::WorkerProto::ReadConn read_conn{.from = source, .version = PROTOCOL_VERSION};
    return nix::WorkerProto::Serialise<nix::store_path_t>::read(store, read_conn);
  };

  BENCHMARK("round-trip ValidPathInfo") {
    sink.clear();
    nix::WorkerProto::write(store, write_conn, info);
    BenchSource source(sink.data());
    nix::WorkerProto::ReadConn read_conn{.from = source, .version = PROTOCOL_VERSION};
    return nix::WorkerProto::Serialise<nix::valid_path_info_t>::read(store, read_conn);
  };

  BENCHMARK("round-trip BuildResult") {
    sink.clear();
    nix::WorkerProto::write(store, write_conn, result);
    BenchSource source(sink.data());
    nix::WorkerProto::ReadConn read_conn{.from = source, .version = PROTOCOL_VERSION};
    return nix::WorkerProto::Serialise<nix::build_result_t>::read(store, read_conn);
  };

  BENCHMARK("round-trip BasicDerivation") {
    sink.clear();
    nix::write_derivation(sink, store, drv);
    BenchSource source(sink.data());
    nix::basic_derivation_t result_drv;
    nix::read_derivation(source, store, result_drv, "test-package-42");
    return result_drv.outputs.size();
  };
}

// =============================================================================
// Benchmark: Typical daemon operation patterns
// =============================================================================

TEST_CASE("Protocol serialization: Typical operation patterns", "[store][protocol][benchmark]") {
  auto store = get_store_config();

  // Simulate queryValidPaths response (common in nix-store -q)
  nix::store_path_set_t closure_paths;
  for (int i = 0; i < 500; ++i) {
    closure_paths.insert(make_store_path(i));
  }

  // Simulate queryPathInfo batch (common in nix copy)
  std::vector<nix::valid_path_info_t> path_infos;
  for (int i = 0; i < 50; ++i) {
    path_infos.push_back(make_valid_path_info(store, i, 10));
  }

  BenchSink sink;
  nix::WorkerProto::WriteConn write_conn{.to = sink, .version = PROTOCOL_VERSION};

  BENCHMARK("queryValidPaths response (500 paths closure)") {
    sink.clear();
    nix::WorkerProto::write(store, write_conn, closure_paths);
    return sink.size();
  };

  BENCHMARK("queryPathInfo batch (50 paths with 10 refs each)") {
    sink.clear();
    for (const auto& info : path_infos) {
      nix::WorkerProto::write(store, write_conn, info);
    }
    return sink.size();
  };

  // Serialize for parsing benchmark
  sink.clear();
  for (const auto& info : path_infos) {
    nix::WorkerProto::write(store, write_conn, info);
  }
  std::string batch_serialized = sink.data();

  BENCHMARK("parse queryPathInfo batch (50 paths)") {
    BenchSource source(batch_serialized);
    nix::WorkerProto::ReadConn read_conn{.from = source, .version = PROTOCOL_VERSION};
    std::vector<nix::valid_path_info_t> results;
    results.reserve(50);
    for (int i = 0; i < 50; ++i) {
      results.push_back(
          nix::WorkerProto::Serialise<nix::valid_path_info_t>::read(store, read_conn));
    }
    return results.size();
  };
}
