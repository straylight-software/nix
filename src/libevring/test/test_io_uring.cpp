// libevring io_uring integration test
//
// tests the same file reader machine against real io_uring I/O

#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>

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

} // namespace

int main() {
  test_read_real_file();
  test_read_nonexistent_file();
  test_read_with_trace();

  std::printf("\nall io_uring tests passed!\n");
  return 0;
}
