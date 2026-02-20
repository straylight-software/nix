# Proposal: Proving the Replayable Ring Can Be Fast

## Thesis

The bulk API is fast not because it abandons the state machine model, but because it uses better
batching. A properly optimized `run()` loop with batching-aware machines should match or exceed bulk
performance while remaining fully replayable.

**Goal:** Prove this with benchmarks, then deprecate the non-replayable bulk internals.

______________________________________________________________________

## Current State

### The `run()` loop today

```cpp
while (!machine.done(state)) {
  auto events = ring.submit_and_wait(1);
  for (auto& e : events) {
    auto [new_state, ops] = machine.step(state, e);
    state = move(new_state);
    for (auto& op : ops) {
      ring.enqueue(op);
    }
  }
}
```

Problems:

1. **Waits for 1 completion** - leaves CQ completions sitting there
2. **Steps one event at a time** - N events = N function calls
3. **Enqueues one op at a time** - no batching awareness
4. **No SQ fill strategy** - doesn't maximize inflight operations

### What bulk does

```cpp
while (completed < total) {
  // Fill SQ to capacity
  while (submitted < total) {
    auto* sqe = io_uring_get_sqe(&ring);
    if (!sqe) break;
    prep_operation(sqe, submitted++);
  }
  
  io_uring_submit_and_wait(&ring, 1);
  
  // Drain entire CQ
  io_uring_for_each_cqe(&ring, head, cqe) {
    handle_completion(cqe);
    count++;
  }
  io_uring_cq_advance(&ring, count);
}
```

The key insight: **keep SQ full, drain CQ completely, minimize syscalls**.

______________________________________________________________________

## Proposal

### 1. Batch-aware `step()` variant

Add a batch step function to the machine concept:

```cpp
template <typename M>
concept batch_machine = machine<M> && requires(
    M m, 
    typename M::state_type s, 
    std::span<const event> events
) {
  // Process multiple events, emit multiple operations
  { m.step_batch(s, events) } -> std::same_as<step_result<typename M::state_type>>;
};
```

Machines can implement `step_batch()` for efficiency, or we provide a default that loops over
`step()`:

```cpp
// Default implementation for machines that don't specialize
template <machine M>
auto step_batch_default(M& m, typename M::state_type state, std::span<const event> events) 
    -> step_result<typename M::state_type> {
  std::vector<operation> all_ops;
  for (const auto& e : events) {
    auto [new_state, ops] = m.step(state, e);
    state = std::move(new_state);
    all_ops.insert(all_ops.end(), ops.begin(), ops.end());
  }
  return {std::move(state), std::move(all_ops)};
}
```

### 2. Optimized `run()` loop

```cpp
template <typename M>
  requires machine<M>
auto run_fast(M& machine, ring& ring) -> typename M::state_type {
  auto state = machine.initial();
  
  // Initial step to get first operations
  auto [s, ops] = machine.step(state, event{});
  state = std::move(s);
  for (const auto& op : ops) {
    ring.enqueue(op);
  }
  
  while (!machine.done(state)) {
    // Submit all enqueued, wait for at least 1
    auto events = ring.submit_and_wait(1);
    
    // But also grab everything else that's ready (non-blocking drain)
    // This requires a ring API addition - see below
    
    // Process all completions in one batch
    if constexpr (batch_machine<M>) {
      auto [new_state, ops] = machine.step_batch(state, events);
      state = std::move(new_state);
      for (const auto& op : ops) {
        ring.enqueue(op);
      }
    } else {
      // Fall back to one-at-a-time
      for (const auto& e : events) {
        auto [new_state, ops] = machine.step(state, e);
        state = std::move(new_state);
        for (const auto& op : ops) {
          ring.enqueue(op);
        }
      }
    }
  }
  
  return state;
}
```

### 3. Ring API additions

The current `submit_and_wait()` returns completions, but we need to drain the entire CQ:

```cpp
class ring {
  // Existing
  virtual auto submit_and_wait(int min_completions = 1) -> std::span<event> = 0;
  
  // New: submit without waiting, return immediately
  virtual auto submit() -> int = 0;
  
  // New: harvest all available completions (non-blocking)
  virtual auto harvest() -> std::span<event> = 0;
  
  // New: how many CQEs are waiting?
  virtual auto ready() const -> std::size_t = 0;
  
  // New: how many SQEs can we enqueue before full?
  virtual auto sq_space() const -> std::size_t = 0;
};
```

This enables:

```cpp
while (!machine.done(state)) {
  // Fill SQ to capacity
  while (ring.sq_space() > 0 && has_more_work(state)) {
    auto [new_state, ops] = machine.step(state, event{});  // or step_ready()
    state = std::move(new_state);
    for (const auto& op : ops) {
      ring.enqueue(op);
    }
  }
  
  // Submit all, wait for at least one
  ring.submit_and_wait(1);
  
  // Drain everything available
  auto events = ring.harvest();
  
  // Process batch
  auto [new_state, ops] = machine.step_batch(state, events);
  state = std::move(new_state);
  for (const auto& op : ops) {
    ring.enqueue(op);
  }
}
```

### 4. "Work generator" pattern for bulk-style machines

For bulk operations (stat 10k files, create 10k files), the machine knows upfront what work it needs
to do. Instead of waiting for completions to generate more ops, it can fill the SQ proactively:

```cpp
struct bulk_stat_machine {
  using state_type = bulk_stat_state;
  
  std::span<const char* const> paths;
  std::span<struct statx> buffers;
  
  auto initial() -> state_type {
    return {.next_to_submit = 0, .completed = 0};
  }
  
  // New: "I have work ready to submit regardless of completions"
  auto has_pending_submissions(const state_type& s) const -> bool {
    return s.next_to_submit < paths.size();
  }
  
  // New: generate operations to fill SQ, no event required
  auto generate(state_type s, std::size_t max_ops) -> step_result<state_type> {
    std::vector<operation> ops;
    ops.reserve(max_ops);
    
    while (ops.size() < max_ops && s.next_to_submit < paths.size()) {
      ops.push_back(operation::make_statx(
          AT_FDCWD, paths[s.next_to_submit], 0, STATX_BASIC_STATS,
          &buffers[s.next_to_submit], s.next_to_submit));
      s.next_to_submit++;
    }
    
    return {std::move(s), std::move(ops)};
  }
  
  // Process completions
  auto step_batch(state_type s, std::span<const event> events) -> step_result<state_type> {
    for (const auto& e : events) {
      s.completed++;
      if (e.result < 0) {
        s.errors.push_back(-e.result);
      }
    }
    return {std::move(s), {}};  // No new ops from completions
  }
  
  auto done(const state_type& s) const -> bool {
    return s.completed >= paths.size();
  }
};
```

And the run loop becomes:

```cpp
template <typename M>
  requires machine<M>
auto run_bulk(M& machine, ring& ring) -> typename M::state_type {
  auto state = machine.initial();
  
  while (!machine.done(state)) {
    // Fill SQ with pending work
    if constexpr (requires { machine.has_pending_submissions(state); }) {
      while (ring.sq_space() > 0 && machine.has_pending_submissions(state)) {
        auto [new_state, ops] = machine.generate(state, ring.sq_space());
        state = std::move(new_state);
        for (const auto& op : ops) {
          ring.enqueue(op);
        }
      }
    }
    
    // Submit and wait
    ring.submit_and_wait(1);
    auto events = ring.harvest();
    
    // Process completions
    auto [new_state, ops] = machine.step_batch(state, events);
    state = std::move(new_state);
    for (const auto& op : ops) {
      ring.enqueue(op);
    }
  }
  
  return state;
}
```

______________________________________________________________________

## Benchmark Plan

### Test cases

1. **stat 10k files** - metadata only, tests syscall overhead
2. **create 10k files** - open+close per file
3. **copy 1GB file** - tests throughput, buffer management
4. **read 10k small files** - mixed open/read/close
5. **copy directory tree** - mixed operations

### Contestants

| Name | Description | |------|-------------| | `posix` | Baseline synchronous syscalls | |
`bulk_current` | Current bulk API (non-replayable) | | `machine_naive` | Current `run()` with naive
machine | | `machine_batch` | New `run_fast()` with batch machine | | `machine_bulk` | New
`run_bulk()` with generator pattern |

### Metrics

- Operations per second
- Time to complete
- CPU cycles per operation (via perf)
- Syscalls per operation (via strace)

### Hypothesis

`machine_bulk` should match `bulk_current` within 5%, while remaining fully replayable.

The overhead of `step()` calls is O(nanoseconds). The kernel round-trip is O(microseconds). As long
as we keep the SQ full and drain the CQ efficiently, the abstraction cost is noise.

______________________________________________________________________

## Implementation Steps

### Phase 1: Ring API additions

```cpp
// In ring.h
virtual auto submit() -> int = 0;
virtual auto harvest() -> std::span<event> = 0;
virtual auto ready() const -> std::size_t = 0;
virtual auto sq_space() const -> std::size_t = 0;

// In io_uring_ring.cpp
auto submit() -> int override {
  return io_uring_submit(&ring_);
}

auto harvest() -> std::span<event> override {
  return harvest_completions();  // Already exists, just expose it
}

auto ready() const -> std::size_t override {
  return io_uring_cq_ready(&ring_);
}

auto sq_space() const -> std::size_t override {
  return io_uring_sq_space_left(&ring_);
}
```

### Phase 2: Batch machine concept

```cpp
// In machine.h
template <typename M>
concept batch_machine = machine<M> && requires(
    M m,
    typename M::state_type s,
    std::span<const event> events
) {
  { m.step_batch(s, events) } -> std::same_as<step_result<typename M::state_type>>;
};

template <typename M>
concept generator_machine = machine<M> && requires(
    M m,
    typename M::state_type const& s,
    std::size_t max_ops
) {
  { m.has_pending_submissions(s) } -> std::same_as<bool>;
  { m.generate(s, max_ops) } -> std::same_as<step_result<typename M::state_type>>;
};
```

### Phase 3: `run_fast()` and `run_bulk()`

New execution functions alongside existing `run()`.

### Phase 4: Rewrite bulk operations as machines

```cpp
// bulk_stat as a machine
auto bulk_stat_machine(std::span<const char* const> paths,
                       std::span<struct statx> buffers) -> /* machine */;

// New bulk_stat implementation
auto bulk_stat(ring& r, std::span<const char* const> paths,
               std::span<struct statx> buffers) -> bulk_result {
  auto machine = bulk_stat_machine(paths, buffers);
  auto final_state = run_bulk(machine, r);
  return final_state.to_result();
}
```

### Phase 5: Benchmark and iterate

Run benchmarks, profile, optimize until `machine_bulk >= bulk_current`.

### Phase 6: Deprecate non-replayable internals

Once proven, remove the private `bulk_context` and raw `io_uring` usage from bulk.cpp.

______________________________________________________________________

## Risks and Mitigations

| Risk | Mitigation | |------|------------| | `step()` overhead adds up | Profile. If hot, consider
`step_batch()` with SIMD-friendly state layout | | Vector allocations in hot path | Pre-allocate in
state, reuse buffers | | Cache misses from state machine indirection | Keep hot state contiguous,
profile cache behavior | | Concept complexity hurts compile times | Keep concepts minimal, test
compile times |

______________________________________________________________________

## Success Criteria

1. **Performance parity:** `machine_bulk` within 5% of `bulk_current` on all benchmarks
2. **Full replayability:** Every bulk operation can be traced and replayed
3. **API simplicity:** No new concepts required for basic usage
4. **Backwards compatible:** Existing machines work unchanged with existing `run()`

______________________________________________________________________

## The Payoff

If this works:

1. **One model, no escape hatches** - Everything is a machine, everything is testable
2. **Bulk operations become debuggable** - Trace a production bulk_stat, replay locally
3. **Simpler codebase** - Remove duplicate io_uring management in bulk.cpp
4. **Confidence for networking** - We know the model scales before building HTTP

______________________________________________________________________

## Next Steps

1. Implement ring API additions (`submit()`, `harvest()`, `ready()`, `sq_space()`)
2. Implement `bulk_stat_machine` as proof of concept
3. Benchmark against current `bulk_stat()`
4. Iterate until parity
5. Convert remaining bulk operations
6. Remove `bulk_context`

Estimated effort: 2-3 days to prove the concept, 1 week to convert all bulk operations.

______________________________________________________________________

## Questions to Resolve

1. **Should `step_batch()` be required or optional?**

   - Leaning optional with default fallback to `step()` loop

2. **Should `generate()` be a separate method or overload of `step()`?**

   - Separate is clearer, but adds API surface

3. **How to handle mixed generate/reactive machines?**

   - HTTP needs both: fill SQ with requests, react to responses
   - Maybe `step()` returns `{state, ops, wants_to_generate: bool}`?

4. **Should replay understand batching?**

   - Current `replay()` feeds events one at a time
   - Should `replay_batch()` exist for batch machines?
   - Or just have `replay()` detect `batch_machine` and use `step_batch()`?
