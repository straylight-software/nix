#pragma once

#include <span>
#include <utility>
#include <vector>

#include "evring/event.h"

namespace evring {

/// result of a step: new state + operations to enqueue
template <typename State>
struct step_result {
  State state;
  std::vector<operation> operations;
};

/// machine concept: the pure functional core
/// implementations define: step(state, event) → (state, [operation])
template <typename M>
concept machine =
    requires(M machine_instance, typename M::state_type state, event completion_event) {
      typename M::state_type;
      { machine_instance.initial() } -> std::same_as<typename M::state_type>;
      {
        machine_instance.step(state, completion_event)
      } -> std::same_as<step_result<typename M::state_type>>;
      { machine_instance.done(state) } -> std::same_as<bool>;
    };

/// replay executor: runs a machine against a recorded event stream
/// this is the key to testability - no I/O, completely deterministic
/// note: the trace captures completion events, not the initial trigger.
/// replay mirrors run(): initial() -> step(empty) -> step(events...)
template <typename M>
  requires machine<M>
auto replay(M& machine_instance, std::span<const event> events) -> typename M::state_type {
  typename M::state_type state = machine_instance.initial();

  // mirror run(): first step with empty event to trigger initial operations
  step_result<typename M::state_type> initial_result = machine_instance.step(state, event{});
  state = std::move(initial_result.state);

  // then process recorded completion events
  for (const event& completion_event : events) {
    step_result<typename M::state_type> result = machine_instance.step(state, completion_event);
    state = std::move(result.state);
    // note: we ignore result.operations in replay - they're just verified
    // or we could collect them for assertions
  }
  return state;
}

/// replay with operation capture: for testing that correct operations are emitted
/// mirrors run(): initial() -> step(empty) -> step(events...)
template <typename M>
  requires machine<M>
auto replay_with_operations(M& machine_instance, std::span<const event> events)
    -> std::pair<typename M::state_type, std::vector<std::vector<operation>>> {
  typename M::state_type state = machine_instance.initial();
  std::vector<std::vector<operation>> all_operations;

  // mirror run(): first step with empty event to trigger initial operations
  step_result<typename M::state_type> initial_result = machine_instance.step(state, event{});
  state = std::move(initial_result.state);
  all_operations.push_back(std::move(initial_result.operations));

  // then process recorded completion events
  for (const event& completion_event : events) {
    step_result<typename M::state_type> result = machine_instance.step(state, completion_event);
    state = std::move(result.state);
    all_operations.push_back(std::move(result.operations));
  }

  return {std::move(state), std::move(all_operations)};
}

/// trace: records events for later replay
class trace {
  std::vector<event> events_;
  // buffer storage for events that own their data
  std::vector<std::vector<std::byte>> buffers_;

public:
  void record(const event& completion_event) {
    if (!completion_event.data.empty()) {
      // copy the data so it survives beyond the original buffer's lifetime
      buffers_.emplace_back(completion_event.data.begin(), completion_event.data.end());
      event copy = completion_event;
      copy.data = std::span<const std::byte>{buffers_.back().data(), buffers_.back().size()};
      events_.push_back(copy);
    } else {
      events_.push_back(completion_event);
    }
  }

  [[nodiscard]] auto events() const noexcept -> std::span<const event> { return events_; }

  void clear() {
    events_.clear();
    buffers_.clear();
  }

  [[nodiscard]] auto size() const noexcept -> std::size_t { return events_.size(); }
};

} // namespace evring
