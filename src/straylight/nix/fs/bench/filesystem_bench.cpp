// straylight::nix::fs benchmarks
//
// Benchmarks for filesystem primitives using nanobench.
// Tests file locking, memory-mapped file access, temp file creation.

#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "../filesystem/file_lock.h"
#include "../filesystem/mmap.h"
#include "../filesystem/temp.h"

#define ANKERL_NANOBENCH_IMPLEMENT
#include <nanobench.h>

namespace fs = straylight::nix::fs;

namespace {

// Generate random data
std::vector<char> generate_random_data(std::size_t size, uint64_t seed = 42) {
  std::mt19937_64 rng(seed);
  std::uniform_int_distribution<int> dist(0, 255);
  std::vector<char> result(size);
  for (auto& c : result) {
    c = static_cast<char>(dist(rng));
  }
  return result;
}

// Create a temp file with data for mmap benchmarks
std::pair<std::filesystem::path, int> create_temp_file_with_data(std::size_t size) {
  auto tmp = fs::TempFile::create("bench");
  if (!tmp) {
    return {{}, -1};
  }

  auto data = generate_random_data(size);
  write(tmp->fd(), data.data(), data.size());
  fsync(tmp->fd());

  auto [fd, path] = tmp->release();
  close(fd); // Close but keep file
  return {path, 0};
}

} // namespace

int main() {
  ankerl::nanobench::Bench bench;
  bench.title("Filesystem Benchmarks").warmup(100).minEpochIterations(100).unit("op");

  // Create a temp directory for all benchmarks
  auto bench_dir = fs::TempDir::create("bench");
  if (!bench_dir) {
    return 1;
  }

  // ───────────────────────────────────────────────────────────────────────────
  // TempFile creation benchmarks
  // ───────────────────────────────────────────────────────────────────────────

  bench.run("TempFile::create", [&] {
    auto tmp = fs::TempFile::create_in(bench_dir->path(), "tmp");
    ankerl::nanobench::doNotOptimizeAway(tmp);
  });

  bench.run("TempFile::create + write 1KB", [&] {
    auto tmp = fs::TempFile::create_in(bench_dir->path(), "tmp");
    if (tmp) {
      char buf[1024] = {};
      write(tmp->fd(), buf, sizeof(buf));
    }
    ankerl::nanobench::doNotOptimizeAway(tmp);
  });

  bench.run("TempFile::create_anonymous", [&] {
    auto tmp = fs::TempFile::create_anonymous(bench_dir->path());
    ankerl::nanobench::doNotOptimizeAway(tmp);
  });

  // ───────────────────────────────────────────────────────────────────────────
  // TempDir creation benchmarks
  // ───────────────────────────────────────────────────────────────────────────

  bench.run("TempDir::create", [&] {
    auto tmp = fs::TempDir::create_in(bench_dir->path(), "dir");
    ankerl::nanobench::doNotOptimizeAway(tmp);
  });

  // ───────────────────────────────────────────────────────────────────────────
  // FileLock benchmarks
  // ───────────────────────────────────────────────────────────────────────────

  auto lock_path = *bench_dir / "bench.lock";

  bench.run("FileLock::exclusive (acquire+release)", [&] {
    auto lock = fs::FileLock::exclusive(lock_path);
    ankerl::nanobench::doNotOptimizeAway(lock);
  });

  bench.run("FileLock::try_exclusive (acquire+release)", [&] {
    auto lock = fs::FileLock::try_exclusive(lock_path);
    ankerl::nanobench::doNotOptimizeAway(lock);
  });

  bench.run("FileLock::shared (acquire+release)", [&] {
    auto lock = fs::FileLock::shared(lock_path);
    ankerl::nanobench::doNotOptimizeAway(lock);
  });

  // Pre-acquire a lock and benchmark upgrade/downgrade
  {
    auto lock = fs::FileLock::shared(lock_path);
    if (lock) {
      bench.run("FileLock::upgrade", [&] {
        lock->upgrade();
        lock->downgrade();
      });
    }
  }

  // ───────────────────────────────────────────────────────────────────────────
  // Memory-mapped file benchmarks
  // ───────────────────────────────────────────────────────────────────────────

  // Create test files of various sizes
  auto [path_1kb, _1] = create_temp_file_with_data(1024);
  auto [path_1mb, _2] = create_temp_file_with_data(1024 * 1024);
  auto [path_10mb, _3] = create_temp_file_with_data(10 * 1024 * 1024);

  bench.run("MappedFileRead::open 1KB", [&] {
    auto mapped = fs::MappedFileRead::open(path_1kb);
    ankerl::nanobench::doNotOptimizeAway(mapped);
  });

  bench.run("MappedFileRead::open 1MB", [&] {
    auto mapped = fs::MappedFileRead::open(path_1mb);
    ankerl::nanobench::doNotOptimizeAway(mapped);
  });

  bench.run("MappedFileRead::open 10MB", [&] {
    auto mapped = fs::MappedFileRead::open(path_10mb);
    ankerl::nanobench::doNotOptimizeAway(mapped);
  });

  // Benchmark sequential read through mmap
  {
    auto mapped = fs::MappedFileRead::open(path_1mb);
    if (mapped) {
      bench.run("mmap sequential read 1MB", [&] {
        volatile uint64_t sum = 0;
        auto data = mapped->data();
        for (std::size_t i = 0; i < data.size(); i += 64) {
          sum += static_cast<uint8_t>(data[i]);
        }
        ankerl::nanobench::doNotOptimizeAway(sum);
      });
    }
  }

  // Benchmark random read through mmap
  {
    auto mapped = fs::MappedFileRead::open(path_1mb);
    if (mapped) {
      std::mt19937_64 rng(12345);
      std::uniform_int_distribution<std::size_t> dist(0, mapped->size() - 1);

      bench.run("mmap random read 1000 accesses", [&] {
        volatile uint64_t sum = 0;
        for (int i = 0; i < 1000; ++i) {
          sum += static_cast<uint8_t>(mapped->data()[dist(rng)]);
        }
        ankerl::nanobench::doNotOptimizeAway(sum);
      });
    }
  }

  // Compare with traditional read()
  {
    int fd = open(path_1mb.c_str(), O_RDONLY);
    if (fd >= 0) {
      bench.run("read() sequential 1MB", [&] {
        lseek(fd, 0, SEEK_SET);
        char buf[4096];
        volatile uint64_t sum = 0;
        ssize_t n;
        while ((n = read(fd, buf, sizeof(buf))) > 0) {
          for (ssize_t i = 0; i < n; i += 64) {
            sum += static_cast<uint8_t>(buf[i]);
          }
        }
        ankerl::nanobench::doNotOptimizeAway(sum);
      });
      close(fd);
    }
  }

  // ───────────────────────────────────────────────────────────────────────────
  // MappedFileWrite benchmarks
  // ───────────────────────────────────────────────────────────────────────────

  {
    auto [write_path, _] = create_temp_file_with_data(1024 * 1024);

    bench.run("MappedFileWrite::open 1MB", [&] {
      auto mapped = fs::MappedFileWrite::open(write_path);
      ankerl::nanobench::doNotOptimizeAway(mapped);
    });

    auto mapped = fs::MappedFileWrite::open(write_path);
    if (mapped) {
      bench.run("mmap write 1MB sequential", [&] {
        auto data = mapped->data();
        for (std::size_t i = 0; i < data.size(); ++i) {
          data[i] = static_cast<std::byte>(i & 0xFF);
        }
        ankerl::nanobench::doNotOptimizeAway(data);
      });
    }

    std::filesystem::remove(write_path);
  }

  // ───────────────────────────────────────────────────────────────────────────
  // Page size query
  // ───────────────────────────────────────────────────────────────────────────

  bench.run("page_size()", [&] {
    auto ps = fs::page_size();
    ankerl::nanobench::doNotOptimizeAway(ps);
  });

  // Cleanup
  std::filesystem::remove(path_1kb);
  std::filesystem::remove(path_1mb);
  std::filesystem::remove(path_10mb);

  return 0;
}
