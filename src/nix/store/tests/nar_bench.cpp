// straylight // nix // store // tests
//
// NAR (Nix Archive) Benchmark Tests
//
// Benchmarks for NAR operations which are the core of store path content handling:
//   1. NAR serialization of directory tree (100 files)
//   2. NAR serialization of large file (100MB)
//   3. NAR deserialization/extraction
//   4. NAR hash calculation
//   5. NAR size calculation

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>

#include "nix/util/archive.h"
#include "nix/util/canon-path.h"
#include "nix/util/fs-sink.h"
#include "nix/util/hash.h"
#include "nix/util/serialise.h"

namespace fs = std::filesystem;

namespace {

// =============================================================================
// Test fixtures
// =============================================================================

/**
 * RAII temporary directory for benchmark fixtures.
 * Uses current directory as base since /tmp may not exist in test sandbox.
 */
struct temp_dir_t {
  fs::path path_;

  temp_dir_t() {
    // Try /tmp first, fall back to current directory
    fs::path base;
    std::error_code ec;
    auto tmp = fs::temp_directory_path(ec);
    if (!ec && fs::exists(tmp, ec)) {
      base = tmp;
    } else {
      base = fs::current_path();
    }
    path_ = base / ("nar_bench_" + std::to_string(std::random_device{}()));
    fs::create_directories(path_);
  }

  ~temp_dir_t() {
    std::error_code ec;
    fs::remove_all(path_, ec);
  }

  temp_dir_t(const temp_dir_t&) = delete;
  temp_dir_t& operator=(const temp_dir_t&) = delete;

  [[nodiscard]] const fs::path& path() const { return path_; }
};

/**
 * Generate deterministic random data for reproducible benchmarks.
 */
std::vector<char> generate_random_data(size_t size, uint64_t seed = 12345) {
  std::mt19937_64 rng(seed);
  std::uniform_int_distribution<int> dist(0, 255);
  std::vector<char> data(size);
  for (auto& c : data) {
    c = static_cast<char>(dist(rng));
  }
  return data;
}

/**
 * Create a directory tree with the specified number of files.
 * Structure: flat directory with N files of varying sizes.
 */
void create_file_tree(const fs::path& root, size_t file_count) {
  fs::create_directories(root);

  std::mt19937_64 rng(42);
  std::uniform_int_distribution<size_t> size_dist(100, 10000); // 100B to 10KB

  for (size_t i = 0; i < file_count; ++i) {
    auto filename = root / ("file_" + std::to_string(i) + ".txt");
    auto data = generate_random_data(size_dist(rng), i);
    std::ofstream ofs(filename, std::ios::binary);
    ofs.write(data.data(), static_cast<std::streamsize>(data.size()));
  }

  // Add some subdirectories for realistic tree structure
  for (size_t i = 0; i < 5; ++i) {
    auto subdir = root / ("subdir_" + std::to_string(i));
    fs::create_directories(subdir);
    for (size_t j = 0; j < 5; ++j) {
      auto filename = subdir / ("nested_" + std::to_string(j) + ".txt");
      auto data = generate_random_data(size_dist(rng), 1000 + i * 10 + j);
      std::ofstream ofs(filename, std::ios::binary);
      ofs.write(data.data(), static_cast<std::streamsize>(data.size()));
    }
  }
}

/**
 * Create a single large file.
 */
void create_large_file(const fs::path& path, size_t size_mb) {
  fs::create_directories(path.parent_path());
  std::ofstream ofs(path, std::ios::binary);

  constexpr size_t chunk_size = 1024 * 1024; // 1MB chunks
  auto chunk = generate_random_data(chunk_size);

  for (size_t i = 0; i < size_mb; ++i) {
    ofs.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
  }
}

/**
 * Counting sink - counts bytes written without storing.
 */
struct counting_sink_t : public nix::sink_t {
  uint64_t bytes_written_ = 0;

  void operator()(std::string_view data) override { bytes_written_ += data.size(); }
  [[nodiscard]] uint64_t bytes_written() const { return bytes_written_; }
  void reset() { bytes_written_ = 0; }
};

/**
 * Memory sink - stores NAR in memory for deserialization benchmark.
 */
struct memory_sink_t : public nix::sink_t {
  std::string buffer_;

  void operator()(std::string_view data) override { buffer_.append(data); }
  [[nodiscard]] const std::string& data() const { return buffer_; }
  void clear() { buffer_.clear(); }
};

/**
 * String source - reads from a string buffer.
 */
struct string_source_t : public nix::source_t {
  const std::string& data_;
  size_t pos_;

  explicit string_source_t(const std::string& data) : data_(data), pos_(0) {}

  size_t read(char* buf, size_t len) override {
    size_t available = data_.size() - pos_;
    size_t to_read = std::min(len, available);
    if (to_read == 0) {
      throw nix::EndOfFile("end of string source");
    }
    std::memcpy(buf, data_.data() + pos_, to_read);
    pos_ += to_read;
    return to_read;
  }

  void reset() { pos_ = 0; }
};

} // namespace

// =============================================================================
// Benchmark: NAR serialization of directory tree (100 files)
// =============================================================================

TEST_CASE("NAR serialization of directory tree (100 files)", "[benchmark][nar]") {
  temp_dir_t tmp;
  auto tree_path = tmp.path() / "tree";
  create_file_tree(tree_path, 100);

  counting_sink_t sink;

  BENCHMARK("dump_path: 100 files + 25 nested") {
    sink.reset();
    nix::dump_path(tree_path.string(), sink);
    return sink.bytes_written();
  };
}

// =============================================================================
// Benchmark: NAR serialization of large file (100MB)
// =============================================================================

TEST_CASE("NAR serialization of large file (100MB)", "[benchmark][nar]") {
  temp_dir_t tmp;
  auto large_file = tmp.path() / "large" / "bigfile.bin";
  create_large_file(large_file, 100);

  counting_sink_t sink;

  BENCHMARK("dump_path: 100MB file") {
    sink.reset();
    nix::dump_path(large_file.parent_path().string(), sink);
    return sink.bytes_written();
  };
}

// =============================================================================
// Benchmark: NAR deserialization/extraction
// =============================================================================

TEST_CASE("NAR deserialization/extraction", "[benchmark][nar]") {
  temp_dir_t tmp;

  // Create source tree and serialize it
  auto tree_path = tmp.path() / "tree";
  create_file_tree(tree_path, 50);

  memory_sink_t nar_data;
  nix::dump_path(tree_path.string(), nar_data);

  auto restore_base = tmp.path() / "restore";
  fs::create_directories(restore_base);
  size_t iteration = 0;

  BENCHMARK("restore_path: 50 files + nested") {
    // Each iteration restores to a new directory, then removes it
    auto restore_path = restore_base / std::to_string(iteration++);

    string_source_t source(nar_data.data());
    nix::restore_path(restore_path.string(), source);

    // Clean up for next iteration
    fs::remove_all(restore_path);

    return true;
  };
}

// =============================================================================
// Benchmark: NAR hash calculation
// =============================================================================

TEST_CASE("NAR hash calculation", "[benchmark][nar]") {
  temp_dir_t tmp;

  // Create a moderate-sized tree
  auto tree_path = tmp.path() / "tree";
  create_file_tree(tree_path, 50);

  BENCHMARK("NAR hash (SHA256) of 50-file tree") {
    nix::hash_sink_t hash_sink(nix::hash_algorithm_t::sha256);
    nix::dump_path(tree_path.string(), hash_sink);
    hash_sink.flush();
    auto result = hash_sink.finish();
    return result.hash.hash_size();
  };
}

TEST_CASE("NAR hash calculation - large file", "[benchmark][nar]") {
  temp_dir_t tmp;

  // Create a 50MB file for hash benchmark
  auto large_file = tmp.path() / "large" / "bigfile.bin";
  create_large_file(large_file, 50);

  BENCHMARK("NAR hash (SHA256) of 50MB file") {
    nix::hash_sink_t hash_sink(nix::hash_algorithm_t::sha256);
    nix::dump_path(large_file.parent_path().string(), hash_sink);
    hash_sink.flush();
    auto result = hash_sink.finish();
    return result.hash.hash_size();
  };
}

// =============================================================================
// Benchmark: NAR size calculation
// =============================================================================

TEST_CASE("NAR size calculation", "[benchmark][nar]") {
  temp_dir_t tmp;

  // Create test trees of different sizes
  auto small_tree = tmp.path() / "small";
  auto medium_tree = tmp.path() / "medium";

  create_file_tree(small_tree, 10);
  create_file_tree(medium_tree, 100);

  counting_sink_t sink;

  BENCHMARK("NAR size: 10-file tree") {
    sink.reset();
    nix::dump_path(small_tree.string(), sink);
    return sink.bytes_written();
  };

  BENCHMARK("NAR size: 100-file tree") {
    sink.reset();
    nix::dump_path(medium_tree.string(), sink);
    return sink.bytes_written();
  };
}

// =============================================================================
// Benchmark: Compare hash algorithms for NAR
// =============================================================================

TEST_CASE("NAR hash algorithm comparison", "[benchmark][nar]") {
  temp_dir_t tmp;

  auto tree_path = tmp.path() / "tree";
  create_file_tree(tree_path, 25);

  BENCHMARK("NAR hash SHA256") {
    nix::hash_sink_t sink(nix::hash_algorithm_t::sha256);
    nix::dump_path(tree_path.string(), sink);
    sink.flush();
    return sink.finish().num_bytes_digested;
  };

  BENCHMARK("NAR hash SHA512") {
    nix::hash_sink_t sink(nix::hash_algorithm_t::sha512);
    nix::dump_path(tree_path.string(), sink);
    sink.flush();
    return sink.finish().num_bytes_digested;
  };

  BENCHMARK("NAR hash SHA1") {
    nix::hash_sink_t sink(nix::hash_algorithm_t::sha1);
    nix::dump_path(tree_path.string(), sink);
    sink.flush();
    return sink.finish().num_bytes_digested;
  };
}

// =============================================================================
// Benchmark: NAR with deep directory structure
// =============================================================================

TEST_CASE("NAR with deep directory structure", "[benchmark][nar]") {
  temp_dir_t tmp;

  // Create a deep directory structure (10 levels, 3 files per level)
  auto deep_path = tmp.path() / "deep";
  auto current = deep_path;
  for (int depth = 0; depth < 10; ++depth) {
    current = current / ("level_" + std::to_string(depth));
    fs::create_directories(current);
    for (int f = 0; f < 3; ++f) {
      auto file = current / ("file_" + std::to_string(f) + ".txt");
      auto data = generate_random_data(1000, depth * 100 + f);
      std::ofstream ofs(file, std::ios::binary);
      ofs.write(data.data(), static_cast<std::streamsize>(data.size()));
    }
  }

  counting_sink_t sink;

  BENCHMARK("dump_path: 10-deep nested structure") {
    sink.reset();
    nix::dump_path(deep_path.string(), sink);
    return sink.bytes_written();
  };
}
