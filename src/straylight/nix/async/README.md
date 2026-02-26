# straylight::nix::async

Work-stealing thread pool and task graph execution built on taskflow.

## Quick Start

```cpp
#include <straylight/nix/async/executor.h>
#include <straylight/nix/async/task_graph.h>
#include <straylight/nix/async/parallel.h>

using namespace straylight::nix::async;

// Work-stealing executor
Executor exec(4);  // 4 worker threads
exec.async([] { do_work(); });
exec.wait_for_all();

// For Boehm GC environments
GcExecutor exec(4);  // Registers worker threads with GC

// Task graph (DAG execution)
TaskGraph graph;
auto a = graph.add_task([] { return fetch_data(); });
auto b = graph.add_task([](auto data) { return process(data); });
graph.add_dependency(a, b);
graph.run(exec);

// Parallel algorithms
parallel_for(items, [](auto& item) { process(item); });
```

## Features

- **Work-stealing scheduler**: Better than FIFO for unbalanced workloads
- **Boehm GC integration**: Worker threads registered with GC via WorkerInterface
- **Task graphs**: DAG-based execution with dependencies
- **Parallel algorithms**: parallel_for, parallel_reduce
- **Async transitive closure**: For Nix dependency resolution

## API Overview

| Type/Function | Description | |---------------|-------------| | `Executor` | Work-stealing thread
pool | | `GcExecutor` | Executor with Boehm GC thread registration | | `TaskGraph` | DAG task
execution | | `parallel_for()` | Parallel iteration | | `parallel_reduce()` | Parallel reduction | |
`async_closure()` | Async transitive closure computation |

## Building

```bash
buck2 build //src/straylight/nix/async:async
buck2 test //src/straylight/nix/async/tests:...
```

## See Also

- [Architecture](docs/ARCHITECTURE.md)
- [Testing](docs/TESTING.md)
