#pragma once

/// @file evring.h
/// @brief libevring: deterministic async I/O with high-performance bulk operations
///
/// libevring provides two complementary APIs:
///
/// 1. **State Machine API** - Deterministic, testable async I/O
///    - Model async operations as pure functions: State × Event → State × [Operation]
///    - Test with replay: evring::replay(machine, captured_events)
///    - Run for real: evring::run(machine, ring)
///
/// 2. **Generator Machines** - Maximum throughput for batch operations
///    - Proactively fill the submission queue for high throughput
///    - Optimized for Nix store operations (stat, copy, create)
///    - Up to 66x faster than POSIX for metadata operations
///    - Fully replayable and testable, unlike the deprecated bulk API
///
/// ## Quick Start
///
/// State machine (testable):
/// @code
///   struct my_machine {
///     using state_type = my_state;
///     auto initial() -> state_type;
///     auto step(state_type, evring::event) -> evring::step_result<state_type>;
///     auto done(state_type const&) -> bool;
///   };
///   auto ring = evring::make_io_uring_ring();
///   auto final_state = evring::run(my_machine{}, *ring);
/// @endcode
///
/// Generator machines (fast + testable):
/// @code
///   auto ring = evring::make_io_uring_ring(256);
///   evring::bulk_stat_machine machine{paths, stat_buffers};
///   auto state = evring::run_generate(machine, *ring);       // 1M+ ops/s
///   // state.succeeded, state.failed, state.errors
/// @endcode
///
/// ## Features
///
/// - **SQPOLL mode**: Kernel-side submission polling for lowest latency
/// - **Fixed file registration**: Reduced per-op overhead for repeated access
/// - **Fixed buffer registration**: Zero-copy I/O with pre-registered memory
/// - **Deterministic replay**: Test async code without actual I/O
///
/// ## Benchmarks (typical NVMe SSD)
///
/// | Operation          | POSIX      | Generator   | Speedup |
/// |--------------------|------------|-------------|---------|
/// | stat 10k files     | 16k ops/s  | 1M+ ops/s   | 66x     |
/// | copy 1GB file      | 1.4 GB/s   | 4.2 GB/s    | 3x      |
/// | create 10k files   | 247k ops/s | 119k ops/s  | 0.5x    |
///
/// @note File creation is slower due to open+close overhead per file.
///       Use bulk_create_machine for best results.

#include "straylight/evring/bulk.h"
#include "straylight/evring/event.h"
#include "straylight/evring/generators.h"
#include "straylight/evring/handle.h"
#include "straylight/evring/http1.h"
#include "straylight/evring/http3.h"
#include "straylight/evring/machine.h"
#include "straylight/evring/ring.h"
