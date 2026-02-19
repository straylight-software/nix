// libevring replay test
//
// demonstrates the core insight: async is trivial to test when modeled
// as a pure function on replay streams

#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <vector>

#include <fcntl.h>

#include "evring/evring.h"

namespace {

// a simple file reader state machine
// reads a file in chunks, accumulates content

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
        // start by opening the file
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
          // EOF - we're done
          state.current_phase = file_reader_state::phase::done;
          operations.push_back(evring::operation::make_close(state.file_handle));
        } else {
          // append data and read more
          state.content.insert(state.content.end(), completion_event.data.begin(),
                               completion_event.data.end());
          operations.push_back(evring::operation::make_read(
              state.file_handle,
              std::span<std::byte>{state.read_buffer.data(), state.read_buffer.size()}));
        }
        break;
      }

      case file_reader_state::phase::done:
      case file_reader_state::phase::error: {
        // terminal states - no more operations
        break;
      }
    }

    return {std::move(state), std::move(operations)};
  }

  auto done(const state_type& state) -> bool {
    return state.current_phase == file_reader_state::phase::done ||
           state.current_phase == file_reader_state::phase::error;
  }
};

// test: replay a successful file read
void test_replay_successful_read() {
  file_reader_machine machine{"/etc/passwd"};

  // captured event stream from a real execution
  evring::handle file_handle{0, 0}; // simulated handle

  std::vector<std::byte> chunk1 = {std::byte{'h'}, std::byte{'e'}, std::byte{'l'}, std::byte{'l'},
                                   std::byte{'o'}};
  std::vector<std::byte> chunk2 = {std::byte{' '}, std::byte{'w'}, std::byte{'o'},
                                   std::byte{'r'}, std::byte{'l'}, std::byte{'d'}};

  // trace events: completion events only (initial step is added by replay)
  std::vector<evring::event> events = {
      // open succeeded, fd = 5 (mapped to handle)
      evring::event{
          .resource_handle = file_handle,
          .operation = evring::operation_type::open,
          .result = 5,
          .data = {},
          .user_data = 0,
      },

      // first read: got "hello"
      evring::event{
          .resource_handle = file_handle,
          .operation = evring::operation_type::read,
          .result = 5,
          .data = std::span<const std::byte>{chunk1},
          .user_data = 0,
      },

      // second read: got " world"
      evring::event{
          .resource_handle = file_handle,
          .operation = evring::operation_type::read,
          .result = 6,
          .data = std::span<const std::byte>{chunk2},
          .user_data = 0,
      },

      // third read: EOF
      evring::event{
          .resource_handle = file_handle,
          .operation = evring::operation_type::read,
          .result = 0,
          .data = {},
          .user_data = 0,
      },

      // close succeeded
      evring::event{
          .resource_handle = file_handle,
          .operation = evring::operation_type::close,
          .result = 0,
          .data = {},
          .user_data = 0,
      },
  };

  // replay the event stream - NO I/O HAPPENS
  file_reader_state final_state = evring::replay(machine, events);

  // verify final state
  assert(final_state.current_phase == file_reader_state::phase::done);
  assert(final_state.content.size() == 11); // "hello world"
  assert(final_state.error_code == 0);

  std::printf("test_replay_successful_read: PASSED\n");
}

// test: replay a failed open
void test_replay_open_failure() {
  file_reader_machine machine{"/nonexistent/file"};

  // trace events: completion events only (initial step is added by replay)
  std::vector<evring::event> events = {
      // open failed with ENOENT
      evring::event{
          .resource_handle = evring::handle::invalid(),
          .operation = evring::operation_type::open,
          .result = -ENOENT,
          .data = {},
          .user_data = 0,
      },
  };

  file_reader_state final_state = evring::replay(machine, events);

  assert(final_state.current_phase == file_reader_state::phase::error);
  assert(final_state.error_code == ENOENT);
  assert(final_state.content.empty());

  std::printf("test_replay_open_failure: PASSED\n");
}

// test: replay with operation capture for verification
void test_replay_with_operations() {
  file_reader_machine machine{"/etc/passwd"};

  evring::handle file_handle{0, 0};

  // trace events: these are COMPLETION events, not including the initial trigger
  // replay_with_operations will add the initial step automatically
  std::vector<evring::event> events = {
      evring::event{
          // open succeeded
          .resource_handle = file_handle,
          .operation = evring::operation_type::open,
          .result = 5,
          .data = {},
          .user_data = 0,
      },
      evring::event{
          // read returned EOF
          .resource_handle = file_handle,
          .operation = evring::operation_type::read,
          .result = 0,
          .data = {},
          .user_data = 0,
      },
      evring::event{
          // close succeeded
          .resource_handle = file_handle,
          .operation = evring::operation_type::close,
          .result = 0,
          .data = {},
          .user_data = 0,
      },
  };

  auto [final_state, all_operations] = evring::replay_with_operations(machine, events);

  // verify correct operations were emitted:
  // initial step (empty event) + 3 completion events = 4 operation sets
  assert(all_operations.size() == 4);

  // step 0 (initial): should emit open
  assert(all_operations[0].size() == 1);
  assert(all_operations[0][0].type == evring::operation_type::open);

  // step 1 (open completion): should emit read
  assert(all_operations[1].size() == 1);
  assert(all_operations[1][0].type == evring::operation_type::read);

  // step 2 (read EOF): should emit close
  assert(all_operations[2].size() == 1);
  assert(all_operations[2][0].type == evring::operation_type::close);

  // step 3 (close completion): no more ops (done state)
  assert(all_operations[3].empty());

  std::printf("test_replay_with_operations: PASSED\n");
}

} // namespace

int main() {
  test_replay_successful_read();
  test_replay_open_failure();
  test_replay_with_operations();

  std::printf("\nall replay tests passed!\n");
  return 0;
}
