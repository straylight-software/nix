// straylight::nix::fs tests
//
// Tests for filesystem primitives: file locking, mmap, temp files/dirs

#include <catch2/catch_test_macros.hpp>
// Catch2 must be included before rapidcheck/catch.h for v3 compatibility
#include <cstring>
#include <fstream>
#include <thread>


#include "straylight/nix/fs/file_lock.h"
#include "straylight/nix/fs/mmap.h"
#include "straylight/nix/fs/temp.h"

namespace fs = straylight::nix::fs;

// ─────────────────────────────────────────────────────────────────────────────
// TempFile tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("TempFile::create creates and removes file", "[temp]") {
  std::filesystem::path saved_path;
  {
    auto tmp = fs::TempFile::create("test");
    REQUIRE(tmp.has_value());
    REQUIRE(tmp->is_open());
    REQUIRE(tmp->fd() >= 0);
    REQUIRE(!tmp->path().empty());
    REQUIRE(std::filesystem::exists(tmp->path()));
    saved_path = tmp->path();
  }
  // File should be deleted after destruction
  REQUIRE_FALSE(std::filesystem::exists(saved_path));
}

TEST_CASE("TempFile::create_in creates file in specific directory", "[temp]") {
  auto parent = fs::TempDir::create("parent");
  REQUIRE(parent.has_value());

  auto tmp = fs::TempFile::create_in(parent->path(), "child");
  REQUIRE(tmp.has_value());
  REQUIRE(tmp->path().parent_path() == parent->path());
}

TEST_CASE("TempFile::release prevents deletion", "[temp]") {
  std::filesystem::path saved_path;
  {
    auto tmp = fs::TempFile::create("release");
    REQUIRE(tmp.has_value());
    saved_path = tmp->path();
    auto [fd, path] = tmp->release();
    REQUIRE(fd >= 0);
    REQUIRE(path == saved_path);
    close(fd); // We own the fd now
  }
  // File should still exist after release
  REQUIRE(std::filesystem::exists(saved_path));
  std::filesystem::remove(saved_path);
}

TEST_CASE("TempFile::keep closes but preserves file", "[temp]") {
  std::filesystem::path saved_path;
  {
    auto tmp = fs::TempFile::create("keep");
    REQUIRE(tmp.has_value());
    saved_path = tmp->path();
    tmp->keep();
    REQUIRE_FALSE(tmp->is_open());
  }
  REQUIRE(std::filesystem::exists(saved_path));
  std::filesystem::remove(saved_path);
}

TEST_CASE("TempFile write and read", "[temp]") {
  auto tmp = fs::TempFile::create("rw");
  REQUIRE(tmp.has_value());

  const char* data = "hello world";
  ssize_t written = write(tmp->fd(), data, strlen(data));
  REQUIRE(written == static_cast<ssize_t>(strlen(data)));

  // Seek back and read
  lseek(tmp->fd(), 0, SEEK_SET);
  char buf[32] = {};
  ssize_t rd = read(tmp->fd(), buf, sizeof(buf) - 1);
  REQUIRE(rd == static_cast<ssize_t>(strlen(data)));
  REQUIRE(std::string(buf) == data);
}

// ─────────────────────────────────────────────────────────────────────────────
// TempDir tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("TempDir::create creates and removes directory", "[temp]") {
  std::filesystem::path saved_path;
  {
    auto tmp = fs::TempDir::create("test");
    REQUIRE(tmp.has_value());
    REQUIRE(tmp->is_valid());
    REQUIRE(!tmp->path().empty());
    REQUIRE(std::filesystem::is_directory(tmp->path()));
    saved_path = tmp->path();
  }
  // Directory should be deleted after destruction
  REQUIRE_FALSE(std::filesystem::exists(saved_path));
}

TEST_CASE("TempDir::operator/ creates subpaths", "[temp]") {
  auto tmp = fs::TempDir::create("subpath");
  REQUIRE(tmp.has_value());

  auto subpath = *tmp / "subdir" / "file.txt";
  REQUIRE(subpath.string().find(tmp->path().string()) == 0);
}

TEST_CASE("TempDir removes contents recursively", "[temp]") {
  std::filesystem::path saved_path;
  {
    auto tmp = fs::TempDir::create("recursive");
    REQUIRE(tmp.has_value());
    saved_path = tmp->path();

    // Create nested structure
    std::filesystem::create_directories(*tmp / "a" / "b" / "c");
    std::ofstream(*tmp / "a" / "file.txt") << "test";
    std::ofstream(*tmp / "a" / "b" / "file2.txt") << "test2";

    REQUIRE(std::filesystem::exists(*tmp / "a" / "b" / "c"));
    REQUIRE(std::filesystem::exists(*tmp / "a" / "b" / "file2.txt"));
  }
  // All should be deleted
  REQUIRE_FALSE(std::filesystem::exists(saved_path));
}

TEST_CASE("TempDir::release prevents deletion", "[temp]") {
  std::filesystem::path saved_path;
  {
    auto tmp = fs::TempDir::create("release");
    REQUIRE(tmp.has_value());
    saved_path = tmp->release();
  }
  REQUIRE(std::filesystem::exists(saved_path));
  std::filesystem::remove_all(saved_path);
}

// ─────────────────────────────────────────────────────────────────────────────
// FileLock tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("FileLock::exclusive acquires lock", "[lock]") {
  auto tmp = fs::TempDir::create("lock");
  REQUIRE(tmp.has_value());
  auto lock_path = *tmp / "test.lock";

  auto lock = fs::FileLock::exclusive(lock_path);
  REQUIRE(lock.has_value());
  REQUIRE(lock->held());
  REQUIRE(lock->mode() == fs::LockMode::Exclusive);
  REQUIRE(lock->path() == lock_path);
  REQUIRE(std::filesystem::exists(lock_path));
}

TEST_CASE("FileLock::shared allows multiple readers", "[lock]") {
  auto tmp = fs::TempDir::create("shared");
  REQUIRE(tmp.has_value());
  auto lock_path = *tmp / "shared.lock";

  auto lock1 = fs::FileLock::shared(lock_path);
  REQUIRE(lock1.has_value());

  auto lock2 = fs::FileLock::shared(lock_path);
  REQUIRE(lock2.has_value());

  // Both should be held
  REQUIRE(lock1->held());
  REQUIRE(lock2->held());
}

TEST_CASE("FileLock::try_exclusive fails if already locked", "[lock]") {
  auto tmp = fs::TempDir::create("trylock");
  REQUIRE(tmp.has_value());
  auto lock_path = *tmp / "try.lock";

  auto lock1 = fs::FileLock::exclusive(lock_path);
  REQUIRE(lock1.has_value());

  // Second exclusive lock should fail (non-blocking)
  auto lock2 = fs::FileLock::try_exclusive(lock_path);
  REQUIRE_FALSE(lock2.has_value());
  REQUIRE(lock2.error() == fs::LockError::WouldBlock);
}

TEST_CASE("FileLock::try_exclusive succeeds after release", "[lock]") {
  auto tmp = fs::TempDir::create("release");
  REQUIRE(tmp.has_value());
  auto lock_path = *tmp / "release.lock";

  {
    auto lock1 = fs::FileLock::exclusive(lock_path);
    REQUIRE(lock1.has_value());
  }
  // lock1 released

  auto lock2 = fs::FileLock::try_exclusive(lock_path);
  REQUIRE(lock2.has_value());
}

TEST_CASE("FileLock upgrade and downgrade", "[lock]") {
  auto tmp = fs::TempDir::create("upgrade");
  REQUIRE(tmp.has_value());
  auto lock_path = *tmp / "upgrade.lock";

  auto lock = fs::FileLock::shared(lock_path);
  REQUIRE(lock.has_value());
  REQUIRE(lock->mode() == fs::LockMode::Shared);

  // Upgrade to exclusive
  auto result = lock->upgrade();
  REQUIRE(result.has_value());
  REQUIRE(lock->mode() == fs::LockMode::Exclusive);

  // Downgrade back to shared
  result = lock->downgrade();
  REQUIRE(result.has_value());
  REQUIRE(lock->mode() == fs::LockMode::Shared);
}

TEST_CASE("FileLock move semantics", "[lock]") {
  auto tmp = fs::TempDir::create("move");
  REQUIRE(tmp.has_value());
  auto lock_path = *tmp / "move.lock";

  auto lock1 = fs::FileLock::exclusive(lock_path);
  REQUIRE(lock1.has_value());

  fs::FileLock lock2 = std::move(*lock1);
  REQUIRE(lock2.held());
  REQUIRE_FALSE(lock1->held());

  // Original should not release on destruction
}

TEST_CASE("FdLock works on existing fd", "[lock]") {
  auto tmp = fs::TempFile::create("fdlock");
  REQUIRE(tmp.has_value());

  auto lock = fs::FdLock::acquire(tmp->fd(), fs::LockMode::Exclusive);
  REQUIRE(lock.has_value());
  REQUIRE(lock->held());

  lock->release();
  REQUIRE_FALSE(lock->held());
}

// ─────────────────────────────────────────────────────────────────────────────
// MappedFile tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("MappedFileRead maps file contents", "[mmap]") {
  auto tmp = fs::TempFile::create("mmap");
  REQUIRE(tmp.has_value());

  const char* data = "hello mmap world";
  [[maybe_unused]] auto n = write(tmp->fd(), data, strlen(data));
  tmp->keep(); // Close fd but keep file

  auto path = tmp->path();
  auto mapped = fs::MappedFileRead::open(path);
  REQUIRE(mapped.has_value());
  REQUIRE(mapped->is_mapped());
  REQUIRE(mapped->size() == strlen(data));
  REQUIRE(mapped->string_view() == data);

  // Cleanup
  std::filesystem::remove(path);
}

TEST_CASE("MappedFileRead::from_fd maps from descriptor", "[mmap]") {
  auto tmp = fs::TempFile::create("mmapfd");
  REQUIRE(tmp.has_value());

  const char* data = "fd mapping";
  [[maybe_unused]] auto n = write(tmp->fd(), data, strlen(data));
  fsync(tmp->fd()); // Ensure data is flushed

  auto mapped = fs::MappedFileRead::from_fd(tmp->fd());
  REQUIRE(mapped.has_value());
  REQUIRE(mapped->string_view() == data);
}

TEST_CASE("MappedFileWrite allows modification", "[mmap]") {
  auto tmp = fs::TempFile::create("mmapwrite");
  REQUIRE(tmp.has_value());

  // Pre-allocate space
  const char* initial = "initial data here";
  [[maybe_unused]] auto n = write(tmp->fd(), initial, strlen(initial));
  tmp->keep();

  auto path = tmp->path();
  auto mapped = fs::MappedFileWrite::open(path);
  REQUIRE(mapped.has_value());

  // Modify the mapping
  auto span = mapped->chars();
  std::memcpy(span.data(), "MODIFIED", 8);
  mapped->sync();
  mapped->unmap();

  // Verify modification persisted
  std::ifstream file(path);
  std::string content;
  std::getline(file, content);
  REQUIRE(content.substr(0, 8) == "MODIFIED");

  std::filesystem::remove(path);
}

TEST_CASE("MappedFileRead returns error for nonexistent file", "[mmap]") {
  auto result = fs::MappedFileRead::open("/nonexistent/path/file.txt");
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error() == fs::MmapError::FileNotFound);
}

TEST_CASE("page_size returns reasonable value", "[mmap]") {
  auto ps = fs::page_size();
  REQUIRE(ps >= 4096);
  REQUIRE(ps <= 65536); // Reasonable upper bound
  // Should be power of 2
  REQUIRE((ps & (ps - 1)) == 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// Property-based tests
// ─────────────────────────────────────────────────────────────────────────────

#include <rapidcheck.h>
#include <rapidcheck/catch.h>

TEST_CASE("TempFile property tests", "[temp][property]") {
  rc::prop("TempFile handles arbitrary data", []() {
    auto tmp = fs::TempFile::create("prop");
    RC_ASSERT(tmp.has_value());

    auto data = *rc::gen::arbitrary<std::vector<char>>();
    if (!data.empty()) {
      ssize_t written = write(tmp->fd(), data.data(), data.size());
      RC_ASSERT(written == static_cast<ssize_t>(data.size()));

      lseek(tmp->fd(), 0, SEEK_SET);
      std::vector<char> buf(data.size());
      ssize_t rd = read(tmp->fd(), buf.data(), buf.size());
      RC_ASSERT(rd == static_cast<ssize_t>(data.size()));
      RC_ASSERT(buf == data);
    }
  });
}

TEST_CASE("MappedFileRead property tests", "[mmap][property]") {
  rc::prop("MappedFileRead correctly maps written data", []() {
    auto tmp = fs::TempFile::create("mmap_prop");
    RC_ASSERT(tmp.has_value());

    auto data = *rc::gen::nonEmpty<std::string>();
    [[maybe_unused]] auto n = write(tmp->fd(), data.data(), data.size());
    fsync(tmp->fd());

    auto mapped = fs::MappedFileRead::from_fd(tmp->fd());
    RC_ASSERT(mapped.has_value());
    RC_ASSERT(mapped->string_view() == data);
  });
}
