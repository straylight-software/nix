// libevring io_uring integration test
//
// tests the same file reader machine against real io_uring I/O

#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "evring/evring.h"

namespace {

// reuse the file reader machine from test_replay.cpp
struct file_reader_state {
  enum class phase {
    initial,
    opening,
    reading,
    done,
    error,
  };

  phase current_phase{phase::initial};
  evring::handle file_handle;
  std::vector<std::byte> content;
  std::vector<std::byte> read_buffer;
  int error_code{0};
};

struct file_reader_machine {
  using state_type = file_reader_state;

  const char* path_;
  std::size_t chunk_size_;

  file_reader_machine(const char* path, std::size_t chunk_size = 4096)
      : path_(path), chunk_size_(chunk_size) {}

  auto initial() -> state_type {
    state_type state;
    state.read_buffer.resize(chunk_size_);
    return state;
  }

  auto step(state_type state, evring::event completion_event) -> evring::step_result<state_type> {
    std::vector<evring::operation> operations;

    switch (state.current_phase) {
      case file_reader_state::phase::initial: {
        state.current_phase = file_reader_state::phase::opening;
        operations.push_back(evring::operation::make_open(path_, O_RDONLY));
        break;
      }

      case file_reader_state::phase::opening: {
        if (!completion_event.ok()) {
          state.current_phase = file_reader_state::phase::error;
          state.error_code = completion_event.error_code();
        } else {
          state.file_handle = completion_event.resource_handle;
          state.current_phase = file_reader_state::phase::reading;
          operations.push_back(evring::operation::make_read(
              state.file_handle,
              std::span<std::byte>{state.read_buffer.data(), state.read_buffer.size()}));
        }
        break;
      }

      case file_reader_state::phase::reading: {
        if (!completion_event.ok()) {
          state.current_phase = file_reader_state::phase::error;
          state.error_code = completion_event.error_code();
        } else if (completion_event.result == 0) {
          state.current_phase = file_reader_state::phase::done;
          operations.push_back(evring::operation::make_close(state.file_handle));
        } else {
          state.content.insert(state.content.end(), completion_event.data.begin(),
                               completion_event.data.end());
          operations.push_back(evring::operation::make_read(
              state.file_handle,
              std::span<std::byte>{state.read_buffer.data(), state.read_buffer.size()}));
        }
        break;
      }

      case file_reader_state::phase::done:
      case file_reader_state::phase::error:
        break;
    }

    return {std::move(state), std::move(operations)};
  }

  auto done(const state_type& state) -> bool {
    return state.current_phase == file_reader_state::phase::done ||
           state.current_phase == file_reader_state::phase::error;
  }
};

void test_read_real_file() {
  file_reader_machine machine{"/etc/hostname"};

  std::unique_ptr<evring::ring> ring_ptr = evring::make_io_uring_ring(64, 0);

  file_reader_state final_state = evring::run(machine, *ring_ptr);

  assert(final_state.current_phase == file_reader_state::phase::done);
  assert(!final_state.content.empty());

  std::string content(reinterpret_cast<const char*>(final_state.content.data()),
                      final_state.content.size());

  std::printf("test_read_real_file: read %zu bytes from /etc/hostname\n",
              final_state.content.size());
  std::printf("  content: %s", content.c_str());
  std::printf("test_read_real_file: PASSED\n");
}

void test_read_nonexistent_file() {
  file_reader_machine machine{"/nonexistent/path/that/does/not/exist"};

  std::unique_ptr<evring::ring> ring_ptr = evring::make_io_uring_ring(64, 0);

  file_reader_state final_state = evring::run(machine, *ring_ptr);

  assert(final_state.current_phase == file_reader_state::phase::error);
  assert(final_state.error_code == ENOENT);
  assert(final_state.content.empty());

  std::printf("test_read_nonexistent_file: correctly got ENOENT\n");
  std::printf("test_read_nonexistent_file: PASSED\n");
}

void test_read_with_trace() {
  file_reader_machine machine{"/etc/hostname"};

  std::unique_ptr<evring::ring> ring_ptr = evring::make_io_uring_ring(64, 0);

  auto [final_state, event_trace] = evring::run_traced(machine, *ring_ptr);

  assert(final_state.current_phase == file_reader_state::phase::done);

  std::printf("test_read_with_trace: captured %zu events\n", event_trace.size());

  // verify we can replay the trace and get the same result
  file_reader_machine replay_machine{"/etc/hostname"};
  file_reader_state replayed_state = evring::replay(replay_machine, event_trace.events());

  assert(replayed_state.current_phase == final_state.current_phase);
  assert(replayed_state.content.size() == final_state.content.size());
  assert(replayed_state.content == final_state.content);

  std::printf("test_read_with_trace: replay produces identical state\n");
  std::printf("test_read_with_trace: PASSED\n");
}

// ============================================================================
// copy_tree tests
// ============================================================================

void test_copy_tree_basic() {
  // create a temp directory structure
  std::string const base = "/tmp/evring_copy_tree_test_" + std::to_string(getpid());
  std::string const source = base + "/source";
  std::string const dest = base + "/dest";

  // cleanup any previous run
  system(("rm -rf " + base).c_str());

  // create source tree:
  // source/
  //   dir1/
  //     file1.txt
  //     file2.txt
  //   dir2/
  //     subdir/
  //       file3.txt
  //   link -> dir1/file1.txt
  //   root_file.txt

  assert(system(("mkdir -p " + source + "/dir1").c_str()) == 0);
  assert(system(("mkdir -p " + source + "/dir2/subdir").c_str()) == 0);
  assert(system(("echo 'content1' > " + source + "/dir1/file1.txt").c_str()) == 0);
  assert(system(("echo 'content2' > " + source + "/dir1/file2.txt").c_str()) == 0);
  assert(system(("echo 'content3' > " + source + "/dir2/subdir/file3.txt").c_str()) == 0);
  assert(system(("echo 'root content' > " + source + "/root_file.txt").c_str()) == 0);
  assert(system(("ln -s dir1/file1.txt " + source + "/link").c_str()) == 0);

  // perform copy
  std::unique_ptr<evring::ring> ring_ptr = evring::make_io_uring_ring(64, 0);

  evring::copy_tree_options options;
  options.preserve_permissions = true;

  auto result = evring::copy_tree(*ring_ptr, source, dest, options);

  std::printf("test_copy_tree_basic: directories=%zu files=%zu symlinks=%zu bytes=%zu failed=%zu\n",
              result.directories_created, result.files_copied, result.symlinks_created,
              result.bytes_copied, result.failed);

  // verify counts
  assert(result.directories_created == 3); // dir1, dir2, dir2/subdir
  assert(result.files_copied == 4);        // 4 regular files
  assert(result.symlinks_created == 1);    // 1 symlink
  assert(result.failed == 0);

  // verify files exist in dest
  struct stat stat_buffer;
  assert(stat((dest + "/dir1/file1.txt").c_str(), &stat_buffer) == 0);
  assert(stat((dest + "/dir1/file2.txt").c_str(), &stat_buffer) == 0);
  assert(stat((dest + "/dir2/subdir/file3.txt").c_str(), &stat_buffer) == 0);
  assert(stat((dest + "/root_file.txt").c_str(), &stat_buffer) == 0);

  // verify symlink
  assert(lstat((dest + "/link").c_str(), &stat_buffer) == 0);
  assert(S_ISLNK(stat_buffer.st_mode));

  char link_target[PATH_MAX];
  ssize_t len = readlink((dest + "/link").c_str(), link_target, sizeof(link_target) - 1);
  assert(len > 0);
  link_target[len] = '\0';
  assert(std::strcmp(link_target, "dir1/file1.txt") == 0);

  // verify file contents
  FILE* fp = fopen((dest + "/dir1/file1.txt").c_str(), "r");
  assert(fp != nullptr);
  char content[256];
  assert(fgets(content, sizeof(content), fp) != nullptr);
  fclose(fp);
  assert(std::strcmp(content, "content1\n") == 0);

  // cleanup
  system(("rm -rf " + base).c_str());

  std::printf("test_copy_tree_basic: PASSED\n");
}

void test_copy_tree_empty_dir() {
  std::string const base = "/tmp/evring_copy_tree_empty_" + std::to_string(getpid());
  std::string const source = base + "/source";
  std::string const dest = base + "/dest";

  system(("rm -rf " + base).c_str());
  assert(system(("mkdir -p " + source).c_str()) == 0);

  std::unique_ptr<evring::ring> ring_ptr = evring::make_io_uring_ring(64, 0);

  auto result = evring::copy_tree(*ring_ptr, source, dest);

  assert(result.directories_created == 0);
  assert(result.files_copied == 0);
  assert(result.symlinks_created == 0);
  assert(result.failed == 0);

  // verify dest was created
  struct stat stat_buffer;
  assert(stat(dest.c_str(), &stat_buffer) == 0);
  assert(S_ISDIR(stat_buffer.st_mode));

  system(("rm -rf " + base).c_str());

  std::printf("test_copy_tree_empty_dir: PASSED\n");
}

void test_copy_tree_many_files() {
  std::string const base = "/tmp/evring_copy_tree_many_" + std::to_string(getpid());
  std::string const source = base + "/source";
  std::string const dest = base + "/dest";

  system(("rm -rf " + base).c_str());
  assert(system(("mkdir -p " + source).c_str()) == 0);

  // create 500 files
  std::size_t const num_files = 500;
  for (std::size_t i = 0; i < num_files; ++i) {
    std::string path = source + "/file_" + std::to_string(i) + ".txt";
    FILE* fp = fopen(path.c_str(), "w");
    assert(fp != nullptr);
    fprintf(fp, "file %zu content\n", i);
    fclose(fp);
  }

  std::unique_ptr<evring::ring> ring_ptr = evring::make_io_uring_ring(64, 0);

  auto result = evring::copy_tree(*ring_ptr, source, dest);

  std::printf("test_copy_tree_many_files: copied %zu files, %zu bytes\n", result.files_copied,
              result.bytes_copied);

  assert(result.files_copied == num_files);
  assert(result.failed == 0);

  // verify random files exist
  struct stat stat_buffer;
  assert(stat((dest + "/file_0.txt").c_str(), &stat_buffer) == 0);
  assert(stat((dest + "/file_250.txt").c_str(), &stat_buffer) == 0);
  assert(stat((dest + "/file_499.txt").c_str(), &stat_buffer) == 0);

  system(("rm -rf " + base).c_str());

  std::printf("test_copy_tree_many_files: PASSED\n");
}

void test_registered_files() {
  // create some test files
  std::string const base = "/tmp/evring_regfiles_" + std::to_string(getpid());
  system(("rm -rf " + base).c_str());
  system(("mkdir -p " + base).c_str());

  std::vector<std::string> paths;
  std::vector<int> fds;
  for (int i = 0; i < 10; ++i) {
    std::string path = base + "/file_" + std::to_string(i);
    int fd = open(path.c_str(), O_CREAT | O_RDWR, 0644);
    assert(fd >= 0);
    // write some data
    std::string data = "test data " + std::to_string(i);
    ssize_t written = write(fd, data.c_str(), data.size());
    assert(written == static_cast<ssize_t>(data.size()));
    lseek(fd, 0, SEEK_SET);
    paths.push_back(path);
    fds.push_back(fd);
  }

  // create ring and register files
  std::unique_ptr<evring::ring> ring_ptr = evring::make_io_uring_ring(64, 0);

  auto reg_files = evring::register_files(*ring_ptr, fds);
  assert(reg_files != nullptr);
  assert(reg_files->capacity() == 10);
  assert(reg_files->size() == 10);

  // verify we can get fds back
  for (std::size_t i = 0; i < fds.size(); ++i) {
    assert(reg_files->get(i) == fds[i]);
  }

  // cleanup
  reg_files.reset(); // unregister
  for (int fd : fds) {
    close(fd);
  }
  system(("rm -rf " + base).c_str());

  std::printf("test_registered_files: PASSED\n");
}

void test_registered_buffers() {
  std::unique_ptr<evring::ring> ring_ptr = evring::make_io_uring_ring(64, 0);

  // allocate aligned buffers
  std::vector<std::unique_ptr<std::byte[], void (*)(void*)>> buffer_storage;
  std::vector<std::span<std::byte>> buffer_spans;

  for (int i = 0; i < 4; ++i) {
    void* ptr = nullptr;
    int result = posix_memalign(&ptr, 4096, 4096);
    assert(result == 0);
    buffer_storage.emplace_back(static_cast<std::byte*>(ptr), free);
    buffer_spans.emplace_back(static_cast<std::byte*>(ptr), 4096);
  }

  auto reg_buffers = evring::register_buffers(*ring_ptr, buffer_spans);
  assert(reg_buffers != nullptr);
  assert(reg_buffers->capacity() == 4);

  // verify buffers are accessible
  for (std::size_t i = 0; i < 4; ++i) {
    auto buf = reg_buffers->get(i);
    assert(buf.data() == buffer_spans[i].data());
    assert(buf.size() == 4096);
  }

  std::printf("test_registered_buffers: PASSED\n");
}

void test_sqpoll_ring() {
  // SQPOLL requires CAP_SYS_NICE or root
  // test that we either succeed or get a clear permission error
  try {
    evring::sqpoll_config config;
    config.idle_milliseconds = 1000;

    std::unique_ptr<evring::ring> ring_ptr =
        evring::make_io_uring_ring(64, evring::ring_flags::sqpoll, config);

    // if we get here, SQPOLL is available - do a simple read test
    file_reader_machine machine{"/etc/hostname"};
    file_reader_state final_state = evring::run(machine, *ring_ptr);

    assert(final_state.current_phase == file_reader_state::phase::done);
    assert(!final_state.content.empty());

    std::printf("test_sqpoll_ring: SQPOLL mode works! Read %zu bytes\n",
                final_state.content.size());
    std::printf("test_sqpoll_ring: PASSED\n");
  } catch (std::runtime_error const& error) {
    // EPERM is expected if we don't have CAP_SYS_NICE
    std::string msg = error.what();
    if (msg.find("Permission denied") != std::string::npos ||
        msg.find("EPERM") != std::string::npos ||
        msg.find("Operation not permitted") != std::string::npos) {
      std::printf("test_sqpoll_ring: SQPOLL not available (need CAP_SYS_NICE or root)\n");
      std::printf("test_sqpoll_ring: SKIPPED (expected)\n");
    } else {
      std::printf("test_sqpoll_ring: unexpected error: %s\n", error.what());
      assert(false);
    }
  }
}

} // namespace

int main() {
  test_read_real_file();
  test_read_nonexistent_file();
  test_read_with_trace();
  test_copy_tree_basic();
  test_copy_tree_empty_dir();
  test_copy_tree_many_files();
  test_registered_files();
  test_registered_buffers();
  test_sqpoll_ring();

  std::printf("\nall io_uring tests passed!\n");
  return 0;
}
