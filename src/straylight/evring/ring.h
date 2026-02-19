#pragma once

#include <memory>
#include <span>

#include "straylight/evring/event.h"
#include "straylight/evring/handle.h"
#include "straylight/evring/machine.h"

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

  /// submit enqueued operations without waiting
  virtual auto submit() -> int = 0;

  /// harvest all available completions (non-blocking)
  virtual auto harvest() -> std::span<event> = 0;

  /// number of completions ready to harvest
  [[nodiscard]] virtual auto cq_ready() const -> std::size_t = 0;

  /// space available in submission queue
  [[nodiscard]] virtual auto sq_space() const -> std::size_t = 0;
};

/// io_uring setup flags (can be OR'd together)
enum class ring_flags : unsigned {
  none = 0,
  sqpoll = 1 << 0,        // kernel-side SQ polling (requires CAP_SYS_NICE or root)
  iopoll = 1 << 1,        // busy-wait for completions (for NVMe/high-perf devices)
  single_issuer = 1 << 2, // single thread submits (optimization hint)
  defer_taskrun = 1 << 3, // defer task work to submit/wait (reduces interrupts)
};

constexpr auto operator|(ring_flags lhs, ring_flags rhs) -> ring_flags {
  return static_cast<ring_flags>(static_cast<unsigned>(lhs) | static_cast<unsigned>(rhs));
}

constexpr auto operator&(ring_flags lhs, ring_flags rhs) -> ring_flags {
  return static_cast<ring_flags>(static_cast<unsigned>(lhs) & static_cast<unsigned>(rhs));
}

constexpr auto operator~(ring_flags flags) -> ring_flags {
  return static_cast<ring_flags>(~static_cast<unsigned>(flags));
}

/// SQPOLL configuration
struct sqpoll_config {
  unsigned idle_milliseconds = 1000; // idle time before kernel thread sleeps
  int cpu = -1;                      // CPU to pin kernel thread to (-1 = no pinning)
};

// ============================================================================
// Fixed file/buffer registration
// ============================================================================

/// registered file table - reduces per-op overhead for frequently used fds
class registered_files {
public:
  virtual ~registered_files() = default;

  /// number of registered file slots
  [[nodiscard]] virtual auto capacity() const -> std::size_t = 0;

  /// number of files currently registered
  [[nodiscard]] virtual auto size() const -> std::size_t = 0;

  /// register a file descriptor, returns slot index or -1 on failure
  virtual auto add(int fd) -> int = 0;

  /// update a slot with a new fd
  virtual auto update(std::size_t slot, int fd) -> bool = 0;

  /// remove a file from a slot
  virtual auto remove(std::size_t slot) -> bool = 0;

  /// get the fd at a slot (-1 if empty)
  [[nodiscard]] virtual auto get(std::size_t slot) const -> int = 0;
};

/// registered buffer table - zero-copy I/O with pre-registered memory
class registered_buffers {
public:
  virtual ~registered_buffers() = default;

  /// number of registered buffer slots
  [[nodiscard]] virtual auto capacity() const -> std::size_t = 0;

  /// get buffer at slot
  [[nodiscard]] virtual auto get(std::size_t slot) const -> std::span<std::byte> = 0;

  /// get all buffers
  [[nodiscard]] virtual auto buffers() const -> std::span<const std::span<std::byte>> = 0;
};

/// create an io_uring-backed ring
auto make_io_uring_ring(unsigned entries = 256, unsigned flags = 0) -> std::unique_ptr<ring>;

/// create an io_uring-backed ring with typed flags
auto make_io_uring_ring(unsigned entries, ring_flags flags, sqpoll_config const& sqpoll = {})
    -> std::unique_ptr<ring>;

/// register files with a ring for reduced per-op overhead
/// returns nullptr if registration fails
auto register_files(ring& ring_instance, std::span<const int> fds)
    -> std::unique_ptr<registered_files>;

/// register files with pre-allocated slots (for dynamic add/remove)
auto register_file_slots(ring& ring_instance, std::size_t num_slots)
    -> std::unique_ptr<registered_files>;

/// register buffers with a ring for zero-copy I/O
/// buffers must remain valid for the lifetime of the returned object
auto register_buffers(ring& ring_instance, std::span<std::span<std::byte>> bufs)
    -> std::unique_ptr<registered_buffers>;

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

/// run a generator machine with maximum throughput
/// keeps SQ full, drains CQ completely - same strategy as bulk API
template <typename M>
  requires generator_machine<M>
auto run_generate(M& machine_instance, ring& ring_instance) -> typename M::state_type {
  typename M::state_type state = machine_instance.initial();

  // helper to enqueue operations
  auto enqueue_ops = [&](std::vector<operation>& ops) {
    for (const operation& op : ops) {
      ring_instance.enqueue(op);
    }
  };

  // helper to process completions
  auto process_completions = [&](std::span<event> events) {
    for (const event& completion_event : events) {
      auto [new_state, ops] = machine_instance.step(state, completion_event);
      state = std::move(new_state);
      enqueue_ops(ops);
    }
  };

  while (!machine_instance.done(state)) {
    // fill SQ with generated work
    while (ring_instance.sq_space() > 0 && machine_instance.wants_to_submit(state)) {
      auto [new_state, ops] = machine_instance.generate(state, ring_instance.sq_space());
      state = std::move(new_state);
      enqueue_ops(ops);
    }

    // nothing to submit and nothing pending? we're stuck or done
    if (ring_instance.pending() == 0) {
      break;
    }

    // submit and wait for at least one completion (also harvests)
    std::span<event> events = ring_instance.submit_and_wait(1);
    process_completions(events);

    // drain any additional completions that are ready
    while (ring_instance.cq_ready() > 0) {
      events = ring_instance.harvest();
      process_completions(events);
    }
  }

  return state;
}

/// run_generate with tracing
template <typename M>
  requires generator_machine<M>
auto run_generate_traced(M& machine_instance, ring& ring_instance)
    -> std::pair<typename M::state_type, trace> {
  trace event_trace;
  typename M::state_type state = machine_instance.initial();

  auto enqueue_ops = [&](std::vector<operation>& ops) {
    for (const operation& op : ops) {
      ring_instance.enqueue(op);
    }
  };

  auto process_completions = [&](std::span<event> events) {
    for (const event& completion_event : events) {
      event_trace.record(completion_event);
      auto [new_state, ops] = machine_instance.step(state, completion_event);
      state = std::move(new_state);
      enqueue_ops(ops);
    }
  };

  while (!machine_instance.done(state)) {
    while (ring_instance.sq_space() > 0 && machine_instance.wants_to_submit(state)) {
      auto [new_state, ops] = machine_instance.generate(state, ring_instance.sq_space());
      state = std::move(new_state);
      enqueue_ops(ops);
    }

    if (ring_instance.pending() == 0) {
      break;
    }

    std::span<event> events = ring_instance.submit_and_wait(1);
    process_completions(events);

    while (ring_instance.cq_ready() > 0) {
      events = ring_instance.harvest();
      process_completions(events);
    }
  }

  return {std::move(state), std::move(event_trace)};
}

} // namespace evring
