#pragma once
///@file

#include "nix/util/file-descriptor.h"
#ifdef _WIN32
#  include "nix/util/windows-async-pipe.h"
#endif

#ifndef _WIN32
#  include <poll.h>
#else
#  include <ioapiset.h>

#  include "nix/util/windows-error.h"
#endif

namespace nix {

/**
 * An "muxable pipe" is a type of pipe supporting endpoints that wait
 * for events on multiple pipes at once.
 *
 * On Unix, this is just a regular anonymous pipe. On Windows, this has
 * to be a named pipe because we need I/O completion_t Ports to wait on
 * multiple pipes.
 */
using muxable_pipe_t =
#ifndef _WIN32
    pipe_t
#else
    windows::AsyncPipe
#endif
    ;

/**
 * use pool() (Unix) / I/O completion_t Ports (Windows) to wait for the
 * input side of any logger pipe to become `available'.  Note that
 * `available' (i.e., non-blocking) includes EOF.
 */
struct muxable_pipe_poll_state_t {
#ifndef _WIN32
  std::vector<struct pollfd> poll_status;
  std::map<int, size_t> fd_to_poll_status;
#else
  OVERLAPPED_ENTRY oentries[0x20] = {0};
  ULONG removed;
  bool gotEOF = false;

#endif

  /**
   * Check for ready (Unix) / completed (Windows) operations
   */
  void poll(
#ifdef _WIN32
      HANDLE ioport,
#endif
      std::optional<unsigned int> timeout);

  using comm_channel_t =
#ifndef _WIN32
      descriptor_t
#else
      windows::AsyncPipe*
#endif
      ;

  /**
   * Process for ready (Unix) / completed (Windows) operations,
   * calling the callbacks as needed.
   *
   * @param handle_read callback to be passed read data.
   *
   * @param handle_eof callback for when the `muxable_pipe_t` has closed.
   */
  void iterate(std::set<comm_channel_t>& channels,
               std::function<void(descriptor_t fd, std::string_view data)> handle_read,
               std::function<void(descriptor_t fd)> handle_eof);
};

} // namespace nix
