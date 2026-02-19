// file_reader.cpp - Read a file using evring state machines
//
// This example demonstrates the fundamental evring pattern:
// 1. Define a state machine with phases
// 2. Implement step() as a pure state transition function
// 3. Run with io_uring for real I/O, or replay for testing
//
// Usage: file_reader <path>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <fcntl.h>

#include "straylight/evring/evring.h"

// ============================================================================
// State: What data the machine needs to track
// ============================================================================

struct file_reader_state {
  enum class phase {
    initial, // Haven't started yet
    opening, // Waiting for open() to complete
    reading, // Waiting for read() to complete
    closing, // Waiting for close() to complete
    done,    // Successfully read file
    error    // Something went wrong
  };

  phase current_phase{phase::initial};
  evring::handle file_handle;

  // Accumulated file content
  std::vector<std::byte> content;

  // Error tracking
  int error_code{0};
  std::string error_message;

  // Helper: is this a terminal state?
  [[nodiscard]] auto finished() const noexcept -> bool {
    return current_phase == phase::done || current_phase == phase::error;
  }
};

// ============================================================================
// Machine: The pure state transition logic
// ============================================================================

class file_reader_machine {
public:
  using state_type = file_reader_state;

  explicit file_reader_machine(const char* path, std::size_t chunk_size = 4096)
      : path_(path), chunk_size_(chunk_size) {}

  // Initial state before any I/O
  [[nodiscard]] auto initial() const -> state_type {
    state_type s;
    s.content.reserve(chunk_size_); // Pre-allocate for efficiency
    return s;
  }

  // Core state transition: (state, event) -> (new_state, operations)
  [[nodiscard]] auto step(state_type s, const evring::event& e) const
      -> evring::step_result<state_type> {
    std::vector<evring::operation> ops;

    switch (s.current_phase) {
      case state_type::phase::initial:
        // First step: open the file
        s.current_phase = state_type::phase::opening;
        ops.push_back(evring::operation::make_open(path_, O_RDONLY));
        break;

      case state_type::phase::opening:
        if (!e.ok()) {
          // Open failed
          s.current_phase = state_type::phase::error;
          s.error_code = e.error_code();
          s.error_message = "Failed to open file";
        } else {
          // Open succeeded - start reading
          s.file_handle = e.resource_handle;
          s.current_phase = state_type::phase::reading;
          // Allocate read buffer in the operation
          ops.push_back(evring::operation::make_read(
              s.file_handle, std::span<std::byte>{read_buffer_.data(), chunk_size_}));
        }
        break;

      case state_type::phase::reading:
        if (!e.ok()) {
          // Read failed
          s.current_phase = state_type::phase::error;
          s.error_code = e.error_code();
          s.error_message = "Failed to read file";
          // Still close the file
          ops.push_back(evring::operation::make_close(s.file_handle));
        } else if (e.result == 0) {
          // EOF reached - close file
          s.current_phase = state_type::phase::closing;
          ops.push_back(evring::operation::make_close(s.file_handle));
        } else {
          // Got some data - accumulate and keep reading
          s.content.insert(s.content.end(), e.data.begin(), e.data.end());
          ops.push_back(evring::operation::make_read(
              s.file_handle, std::span<std::byte>{read_buffer_.data(), chunk_size_}));
        }
        break;

      case state_type::phase::closing:
        // Close completed (we don't care if it fails)
        s.current_phase = state_type::phase::done;
        break;

      case state_type::phase::done:
      case state_type::phase::error:
        // Terminal states - nothing to do
        break;
    }

    return {std::move(s), std::move(ops)};
  }

  // Termination condition
  [[nodiscard]] auto done(const state_type& s) const -> bool { return s.finished(); }

private:
  const char* path_;
  std::size_t chunk_size_;
  // Buffer for reads (mutable because step() is const but we need storage)
  mutable std::vector<std::byte> read_buffer_{std::vector<std::byte>(4096)};
};

// ============================================================================
// Main: Show both real execution and replay
// ============================================================================

int main(int argc, char* argv[]) {
  if (argc < 2) {
    std::fprintf(stderr, "Usage: %s <path>\n", argv[0]);
    return 1;
  }

  const char* path = argv[1];

  // Create io_uring ring
  auto ring = evring::make_io_uring_ring(64);
  if (!ring) {
    std::fprintf(stderr, "Failed to create io_uring\n");
    return 1;
  }

  // Create and run the machine
  file_reader_machine reader{path};

  std::printf("Reading file: %s\n", path);

  // Run with tracing so we can replay later
  auto [final_state, trace] = evring::run_traced(reader, *ring);

  if (final_state.current_phase == file_reader_state::phase::error) {
    std::fprintf(stderr, "Error: %s (errno=%d)\n", final_state.error_message.c_str(),
                 final_state.error_code);
    return 1;
  }

  // Show results
  std::printf("Read %zu bytes\n", final_state.content.size());

  // Print content if it's text and not too large
  if (final_state.content.size() <= 1024) {
    std::printf("Content:\n---\n");
    std::fwrite(final_state.content.data(), 1, final_state.content.size(), stdout);
    if (!final_state.content.empty() && static_cast<char>(final_state.content.back()) != '\n') {
      std::printf("\n");
    }
    std::printf("---\n");
  }

  // Demonstrate replay (same machine, recorded events, no I/O)
  std::printf("\nReplaying from %zu captured events...\n", trace.size());
  auto replayed_state = evring::replay(reader, trace.events());

  if (replayed_state.content.size() == final_state.content.size()) {
    std::printf("Replay produced identical result!\n");
  } else {
    std::printf("Replay mismatch: %zu vs %zu bytes\n", replayed_state.content.size(),
                final_state.content.size());
  }

  return 0;
}
