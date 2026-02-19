#pragma once

#include <memory>
#include <span>

#include "evring/event.h"
#include "evring/handle.h"
#include "evring/machine.h"

namespace evring {

enum class resource_type : std::uint8_t {
  file,
  socket,
  timer,
};

/// resource info stored per handle
struct resource {
  int file_descriptor{-1};
  resource_type type{resource_type::file};
  bool closing{false};
};

/// ring: abstract interface for I/O submission/completion
class ring {
public:
  ring() = default;
  virtual ~ring() = default;

  // non-copyable, non-movable (virtual destructor implies polymorphic use)
  ring(const ring&) = delete;
  ring& operator=(const ring&) = delete;
  ring(ring&&) = delete;
  ring& operator=(ring&&) = delete;

  /// enqueue an operation (does not submit yet)
  virtual void enqueue(const operation& operation_to_enqueue) = 0;

  /// submit all enqueued operations and wait for at least min_completions
  virtual auto submit_and_wait(int min_completions = 1) -> std::span<event> = 0;

  /// non-blocking poll for completions
  virtual auto poll() -> std::span<event> = 0;

  /// allocate a handle for a file descriptor (for external fds)
  virtual auto register_file_descriptor(int file_descriptor,
                                        resource_type type = resource_type::file) -> handle = 0;

  /// get the fd for a handle
  [[nodiscard]] virtual auto get_file_descriptor(handle resource_handle) const -> int = 0;

  /// number of pending operations
  [[nodiscard]] virtual auto pending() const -> std::size_t = 0;

  /// number of active handles
  [[nodiscard]] virtual auto active_handles() const -> std::size_t = 0;
};

/// create an io_uring-backed ring
auto make_io_uring_ring(unsigned entries = 256, unsigned flags = 0) -> std::unique_ptr<ring>;

/// run a machine against a ring until done
template <typename M>
  requires machine<M>
auto run(M& machine_instance, ring& ring_instance) -> typename M::state_type {
  typename M::state_type state = machine_instance.initial();

  // enqueue initial operations (empty event triggers initial state)
  step_result<typename M::state_type> initial_result = machine_instance.step(state, event{});
  state = std::move(initial_result.state);
  for (const operation& operation_to_enqueue : initial_result.operations) {
    ring_instance.enqueue(operation_to_enqueue);
  }

  while (!machine_instance.done(state)) {
    std::span<event> events = ring_instance.submit_and_wait(1);
    for (const event& completion_event : events) {
      step_result<typename M::state_type> result = machine_instance.step(state, completion_event);
      state = std::move(result.state);
      for (const operation& operation_to_enqueue : result.operations) {
        ring_instance.enqueue(operation_to_enqueue);
      }
    }
  }

  return state;
}

/// run with tracing: records all events for replay
template <typename M>
  requires machine<M>
auto run_traced(M& machine_instance, ring& ring_instance)
    -> std::pair<typename M::state_type, trace> {
  trace event_trace;
  typename M::state_type state = machine_instance.initial();

  // enqueue initial operations
  step_result<typename M::state_type> initial_result = machine_instance.step(state, event{});
  state = std::move(initial_result.state);
  for (const operation& operation_to_enqueue : initial_result.operations) {
    ring_instance.enqueue(operation_to_enqueue);
  }

  while (!machine_instance.done(state)) {
    std::span<event> events = ring_instance.submit_and_wait(1);
    for (const event& completion_event : events) {
      event_trace.record(completion_event);
      step_result<typename M::state_type> result = machine_instance.step(state, completion_event);
      state = std::move(result.state);
      for (const operation& operation_to_enqueue : result.operations) {
        ring_instance.enqueue(operation_to_enqueue);
      }
    }
  }

  return {std::move(state), std::move(event_trace)};
}

} // namespace evring
