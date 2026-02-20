#pragma once
///@file

#include "nix/util/logging.h"
#include "nix/util/processes.h"
#include "nix/util/serialise.h"

namespace nix {

/**
 * @note Sometimes this is owned by the `Worker`, and sometimes it is
 * owned by a `Goal`. This is for efficiency: rather than starting the
 * hook every time we want to ask whether we can run a remote build
 * (which can be very often), we reuse a hook process for answering
 * those queries until it accepts a build.  So if there are N
 * derivations to be built, at most N hooks will be started.
 */
struct HookInstance {
  /**
   * Pipes for talking to the build hook.
   */
  pipe_t toHook;

  /**
   * pipe_t for the hook's standard output/error.
   */
  pipe_t fromHook;

  /**
   * pipe_t for the builder's standard output/error.
   */
  pipe_t builder_out;

  /**
   * The process ID of the hook.
   */
  process_handle_t pid;

  /**
   * The remote machine on which we're building.
   *
   * @Invariant When the hook instance is owned by the `Worker`, this
   * is the empty string. When it is owned by a `Goal`, this should be
   * set.
   */
  std::string machine_name;

  fd_sink_t sink;

  std::map<activity_id_t, activity_t> activities;

  HookInstance();

  ~HookInstance();
};

} // namespace nix
