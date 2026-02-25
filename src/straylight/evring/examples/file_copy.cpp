// file_copy.cpp - Copy a file with progress reporting
//
// This example shows:
// - Multi-resource state machine (source and dest file handles)
// - Progress reporting during I/O
// - Error handling with cleanup
//
// Usage: file_copy <source> <dest>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <fcntl.h>

#include "straylight/evring/evring.h"

// ============================================================================
// State
// ============================================================================

struct file_copy_state {
  enum class phase {
    initial,
    opening_source,
    opening_dest,
    reading,
    writing,
    closing_source,
    closing_dest,
    done,
    error
  };

  phase current_phase{phase::initial};

  // File handles
  evring::handle source_handle;
  evring::handle dest_handle;

  // Size of data in buffer (buffer is in machine, not state)
  std::size_t bytes_in_buffer{0};

  // Progress
  std::size_t total_bytes_copied{0};

  // Error info
  int error_code{0};
  std::string error_message;
};

// ============================================================================
// Machine
// ============================================================================

struct file_copy_machine {
  using state_type = file_copy_state;

  file_copy_machine(const char* source, const char* dest, std::size_t buffer_size = 64 * 1024)
      : source_(source), dest_(dest), buffer_(buffer_size) {}

  [[nodiscard]] auto initial() const -> state_type { return {}; }

  // Provide stable span for read buffer (buffer is in machine, not state)
  [[nodiscard]] auto read_buffer_span() const -> evring::stable_span<std::byte> {
    return evring::make_stable_span(std::span{buffer_.data(), buffer_.size()});
  }

  [[nodiscard]] auto step(state_type s, const evring::event& e) const
      -> evring::step_result<state_type> {
    std::vector<evring::operation> ops;

    switch (s.current_phase) {
      case state_type::phase::initial:
        s.current_phase = state_type::phase::opening_source;
        ops.push_back(evring::operation::make_open(source_, O_RDONLY));
        break;

      case state_type::phase::opening_source:
        if (!e.ok()) {
          s.current_phase = state_type::phase::error;
          s.error_code = e.error_code();
          s.error_message = "Failed to open source file";
        } else {
          s.source_handle = e.resource_handle;
          s.current_phase = state_type::phase::opening_dest;
          ops.push_back(evring::operation::make_open(dest_, O_WRONLY | O_CREAT | O_TRUNC, 0644));
        }
        break;

      case state_type::phase::opening_dest:
        if (!e.ok()) {
          s.current_phase = state_type::phase::error;
          s.error_code = e.error_code();
          s.error_message = "Failed to create destination file";
          // Clean up source
          ops.push_back(evring::operation::make_close(s.source_handle));
        } else {
          s.dest_handle = e.resource_handle;
          s.current_phase = state_type::phase::reading;
          ops.push_back(evring::operation::make_read(s.source_handle, read_buffer_span()));
        }
        break;

      case state_type::phase::reading:
        if (!e.ok()) {
          s.current_phase = state_type::phase::error;
          s.error_code = e.error_code();
          s.error_message = "Read error";
          // Clean up both files
          ops.push_back(evring::operation::make_close(s.source_handle));
          ops.push_back(evring::operation::make_close(s.dest_handle));
        } else if (e.result == 0) {
          // EOF - done copying
          s.current_phase = state_type::phase::closing_source;
          ops.push_back(evring::operation::make_close(s.source_handle));
        } else {
          // Got data - write it
          s.bytes_in_buffer = static_cast<std::size_t>(e.result);
          s.current_phase = state_type::phase::writing;
          ops.push_back(evring::operation::make_write(
              s.dest_handle, std::span{buffer_.data(), s.bytes_in_buffer}));
        }
        break;

      case state_type::phase::writing:
        if (!e.ok()) {
          s.current_phase = state_type::phase::error;
          s.error_code = e.error_code();
          s.error_message = "Write error";
          ops.push_back(evring::operation::make_close(s.source_handle));
          ops.push_back(evring::operation::make_close(s.dest_handle));
        } else {
          s.total_bytes_copied += static_cast<std::size_t>(e.result);
          // Print progress
          std::printf("\rCopied %zu bytes...", s.total_bytes_copied);
          std::fflush(stdout);
          // Read more
          s.current_phase = state_type::phase::reading;
          ops.push_back(evring::operation::make_read(s.source_handle, read_buffer_span()));
        }
        break;

      case state_type::phase::closing_source:
        s.current_phase = state_type::phase::closing_dest;
        ops.push_back(evring::operation::make_close(s.dest_handle));
        break;

      case state_type::phase::closing_dest:
        s.current_phase = state_type::phase::done;
        break;

      case state_type::phase::done:
      case state_type::phase::error:
        break;
    }

    return {std::move(s), std::move(ops)};
  }

  [[nodiscard]] auto done(const state_type& s) const -> bool {
    return s.current_phase == state_type::phase::done ||
           s.current_phase == state_type::phase::error;
  }

  const char* source_;
  const char* dest_;
  mutable std::vector<std::byte> buffer_;
};

// ============================================================================
// Main
// ============================================================================

int main(int argc, char* argv[]) {
  if (argc != 3) {
    std::fprintf(stderr, "Usage: %s <source> <dest>\n", argv[0]);
    return 1;
  }

  auto ring = evring::make_io_uring_ring(64);
  if (!ring) {
    std::fprintf(stderr, "Failed to create io_uring\n");
    return 1;
  }

  file_copy_machine copier{argv[1], argv[2]};
  auto state = evring::run(copier, *ring);

  std::printf("\n"); // End the progress line

  if (state.current_phase == file_copy_state::phase::error) {
    std::fprintf(stderr, "Error: %s (errno=%d)\n", state.error_message.c_str(), state.error_code);
    return 1;
  }

  std::printf("Successfully copied %zu bytes from %s to %s\n", state.total_bytes_copied, argv[1],
              argv[2]);
  return 0;
}
