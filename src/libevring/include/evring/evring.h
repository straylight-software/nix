#pragma once

// libevring: deterministic async I/O via replay streams
//
// the core insight: async is trivial to test if you model it as a pure
// function on well-defined replay streams.
//
// State × Event → State × [Operation]
//
// usage:
//   1. define your state type
//   2. implement initial(), step(), done() functions
//   3. run against real I/O with evring::run(machine, ring)
//   4. test with evring::replay(machine, captured_events)
//
// example:
//
//   struct file_copy_state {
//       bool source_opened = false;
//       bool destination_opened = false;
//       bool done = false;
//       evring::handle source_handle;
//       evring::handle destination_handle;
//       std::vector<std::byte> buffer;
//   };
//
//   struct file_copy_machine {
//       using state_type = file_copy_state;
//
//       auto initial() -> state_type { return {}; }
//
//       auto step(state_type state, evring::event event) -> evring::step_result<state_type> {
//           std::vector<evring::operation> operations;
//           // ... state machine logic
//           return {std::move(state), std::move(operations)};
//       }
//
//       auto done(const state_type& state) -> bool { return state.done; }
//   };

#include "evring/bulk.h"
#include "evring/event.h"
#include "evring/handle.h"
#include "evring/machine.h"
#include "evring/ring.h"
