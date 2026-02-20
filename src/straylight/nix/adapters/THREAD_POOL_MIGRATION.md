# Migration Guide: Nix ThreadPool to straylight::nix::primitives::async

This document describes how to migrate Nix code from the legacy `nix::ThreadPool` and
`nix::processGraph` to the new `straylight::nix::primitives::async` primitives.

## Overview

### Old API (nix::ThreadPool)

```cpp
#include "nix/util/thread-pool.h"

nix::ThreadPool pool(4);
pool.enqueue([]{ work(); });
pool.process();

// Or for graph processing:
nix::processGraph<StorePath>(nodes, getEdges, processNode, discoverNodes, maxThreads);
```

### New API (straylight primitives)

```cpp
#include "straylight/nix/primitives/async/executor.h"
#include "straylight/nix/primitives/async/task_graph.h"

using namespace straylight::nix::primitives::async;

Executor exec(4);
exec.silent_async([]{ work(); });
exec.wait_for_all();

// Or for graph processing:
process_graph<StorePath>(nodes, getEdges, processNode, exec, discoverNodes);
```

### Compatibility Adapter

For incremental migration, use the adapter:

```cpp
#include "straylight/nix/primitives/adapters/thread_pool_adapter.h"

using namespace straylight::nix::primitives::adapters;

ThreadPool pool(4);           // Same interface as nix::ThreadPool
pool.enqueue([]{ work(); });
pool.process();

// Graph processing with same signature:
processGraph<StorePath>(nodes, getEdges, processNode, discoverNodes, maxThreads);
```

______________________________________________________________________

## API Mapping

### ThreadPool Methods

| nix::ThreadPool | straylight Executor | Adapter |
|--------------------------|---------------------------|---------------------------| |
`ThreadPool(n)` | `Executor(n)` | `ThreadPool(n)` | | `enqueue(work)` | `silent_async(work)` |
`enqueue(work)` | | `process()` | `wait_for_all()` | `process()` | | `shutdown()` | destructor
handles it | `shutdown()` | | `ThreadPoolShutDown` | N/A (throws std::runtime) |
`ThreadPoolShutDown` |

### Key Behavioral Differences

1. **Scheduling Strategy**

   - Old: FIFO queue - tasks execute in submission order
   - New: Work-stealing - better load balancing for unbalanced workloads
   - Impact: Task execution order may differ, but correctness should be preserved

2. **Thread Creation**

   - Old: Lazy - threads created as needed up to max
   - New: Eager - all threads created at construction
   - Impact: Slightly more resource usage at startup, but better steady-state performance

3. **Exception Handling**

   - Old: First exception stored, others printed to stderr
   - New: First exception stored, others silently ignored
   - Impact: Less noisy output, but may lose some debugging info

4. **Return Values**

   - Old: `enqueue()` returns void
   - New: `async()` returns `std::future<T>` for the result
   - Impact: New API is more flexible; use `silent_async()` for old behavior

### processGraph

| nix::processGraph | straylight process_graph |
|------------------------------------------|-----------------------------------| |
`processGraph<T>(nodes, getEdges, ...)` | `process_graph<T>(nodes, ...)` | | `discoverNodes`
parameter | `discover` parameter | | `maxThreads` parameter | Uses passed executor's threads |

______________________________________________________________________

## Migration Strategy

### Phase 1: Use Adapters (Low Risk)

Replace includes and add namespace qualification:

```cpp
// Before:
#include "nix/util/thread-pool.h"
nix::ThreadPool pool(4);

// After:
#include "straylight/nix/primitives/adapters/thread_pool_adapter.h"
straylight::nix::primitives::adapters::ThreadPool pool(4);
```

Or add a using declaration:

```cpp
#include "straylight/nix/primitives/adapters/thread_pool_adapter.h"
using straylight::nix::primitives::adapters::ThreadPool;
```

### Phase 2: Direct Migration (Recommended)

Migrate to the new API directly for new code and gradually for existing code.

```cpp
#include "straylight/nix/primitives/async/executor.h"

using namespace straylight::nix::primitives::async;

Executor exec(4);

// Instead of pool.enqueue() / pool.process():
for (auto& item : items) {
    exec.silent_async([&, item]{ process(item); });
}
exec.wait_for_all();
```

For graph processing:

```cpp
#include "straylight/nix/primitives/async/task_graph.h"

using namespace straylight::nix::primitives::async;

Executor exec(4);
process_graph<StorePath>(
    nodes,
    [&](const StorePath& p) { return getReferences(p); },
    [&](const StorePath& p) { processPath(p); },
    exec,
    discoverNodes
);
```

______________________________________________________________________

## Boehm GC Integration

The original Nix ThreadPool does NOT register worker threads with Boehm GC. This is safe because Nix
typically uses ThreadPool for I/O-bound work where GC-managed objects aren't accessed from worker
threads.

However, if you're accessing Nix values (which are GC-managed) from worker threads, you MUST use the
GC-aware variants:

### Using the Adapter

```cpp
#include "straylight/nix/primitives/adapters/thread_pool_adapter.h"

// Use GcThreadPool instead of ThreadPool:
straylight::nix::primitives::adapters::GcThreadPool pool(4);

// Or for processGraph:
straylight::nix::primitives::adapters::processGraphGc<T>(
    nodes, getEdges, processNode, discoverNodes, maxThreads);
```

### Using the New API Directly

```cpp
#include "straylight/nix/primitives/async/executor.h"

// Use GcExecutor instead of Executor:
straylight::nix::primitives::async::GcExecutor exec(4);
```

### When to Use GC-Aware Execution

Use `GcExecutor` / `GcThreadPool` when:

- Processing Nix expressions (Value\*)
- Accessing the Nix evaluator state from workers
- Working with any GC-allocated Nix objects

Use regular `Executor` / `ThreadPool` when:

- Doing I/O operations (file reads, network requests)
- Processing store paths (StorePath is not GC-managed)
- Working with derivations (Derivation is not GC-managed)

______________________________________________________________________

## Files Requiring Migration

Based on the current codebase, these files use ThreadPool:

### src/nix/store/store-api.cpp

```cpp
// Current usage (line ~717):
ThreadPool pool(maxThreads);
pool.enqueue(std::bind(doQuery, path));
pool.process();

// Suggested migration:
Executor exec(maxThreads);
for (auto& path : paths) {
    exec.silent_async([&, path]{ doQuery(path); });
}
exec.wait_for_all();
```

### src/nix/store/misc.cpp

```cpp
// Current usage: Complex callback-based pattern with dynamic discovery
ThreadPool pool(maxThreads);
pool.enqueue(std::bind(do_path, DerivedPath::Built{...}));

// Suggested migration: Keep adapter for now, or refactor to process_graph
// The pattern here closely matches process_graph with discovery enabled
```

### src/nix/cli/verify.cpp

```cpp
// Current usage (line ~170):
ThreadPool pool(maxThreads);
for (auto& store_path : store_paths)
    pool.enqueue(std::bind(do_path, store_path));
pool.process();

// Suggested migration:
Executor exec(maxThreads);
for (auto& store_path : store_paths) {
    exec.silent_async([&, store_path]{ do_path(store_path); });
}
exec.wait_for_all();
```

### src/nix/cli/sigs.cpp

```cpp
// Similar pattern to verify.cpp - straightforward migration
```

### src/nix/cli/flake-prefetch-inputs.cpp

```cpp
// Current usage: Recursive tree traversal with pool.enqueue
// Consider: parallel_for or process_graph depending on structure
```

### src/nix/fetchers/git-utils.cpp

```cpp
// Current usage: Git history traversal (commit graph)
// Consider: process_graph for commit DAG traversal
```

______________________________________________________________________

## Testing Migration

After migrating a file:

1. **Run unit tests** for that component
2. **Check for race conditions** - work-stealing may expose latent bugs
3. **Verify exception behavior** - ensure errors are still properly reported
4. **Profile if performance-critical** - work-stealing should be faster for unbalanced workloads,
   but may have slightly higher overhead for trivial tasks

______________________________________________________________________

## Advanced: Using TaskGraph for Static DAGs

If you have a DAG where all edges are known upfront (not discovered during processing), use
`TaskGraph` for better performance:

```cpp
#include "straylight/nix/primitives/async/task_graph.h"

using namespace straylight::nix::primitives::async;

TaskGraph<StorePath> graph;

// Add all nodes with their work
for (const auto& path : paths) {
    graph.add_node(path, [&, path]{ processPath(path); });
}

// Add edges (dependencies)
for (const auto& [path, deps] : dependencies) {
    for (const auto& dep : deps) {
        graph.add_edge(dep, path);  // dep must complete before path
    }
}

// Execute
Executor exec(4);
graph.execute(exec);
```

This is more efficient than `process_graph` when edges are known upfront because taskflow can
optimize the execution order.

______________________________________________________________________

## Advanced: Parallel Algorithms

For simple parallel iteration without DAG dependencies, use the parallel algorithms in `parallel.h`:

```cpp
#include "straylight/nix/primitives/async/parallel.h"

using namespace straylight::nix::primitives::async;

Executor exec(4);

// Instead of:
// for (auto& path : paths) pool.enqueue([&]{ process(path); });
// pool.process();

// Use:
parallel_for(exec, paths, [&](const auto& path) {
    process(path);
});

// Or with index:
parallel_for_index(exec, paths.size(), [&](std::size_t i) {
    process(paths[i]);
});
```

______________________________________________________________________

## Summary Checklist

- [ ] Replace `#include "nix/util/thread-pool.h"` with adapter or new header
- [ ] Update namespace qualifications
- [ ] For GC-sensitive code, use `GcExecutor` / `GcThreadPool`
- [ ] Consider using `process_graph` for DAG patterns
- [ ] Consider using `TaskGraph` for static DAGs
- [ ] Consider using `parallel_for` for simple parallel iteration
- [ ] Run tests and verify behavior
- [ ] Profile if performance matters
