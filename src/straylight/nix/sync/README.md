# straylight::nix::sync

Thread synchronization primitives with RAII-based locking.

## Quick Start

```cpp
#include <straylight/nix/sync/synchronized.h>
#include <straylight/nix/sync/pool.h>
#include <straylight/nix/sync/signals.h>

using namespace straylight::nix::sync;

// Synchronized value (like folly::Synchronized)
Sync<Data> data;
{
    auto lock = data.lock();
    lock->x = 42;
}

// Lambda-based access
data.with_lock([](Data& d) { d.x = 42; });

// Reader-writer synchronization
SharedSync<Data> shared_data;
{
    auto read = shared_data.read_lock();   // Multiple readers
    std::cout << read->x;
}
{
    auto write = shared_data.write_lock(); // Exclusive writer
    write->x = 42;
}

// Resource pool
Pool<Connection> pool(create_connection, 10);
auto conn = pool.acquire();  // RAII handle

// Interrupt signals
InterruptSignal signal;
signal.raise();
if (signal.is_raised()) { /* cleanup */ }
```

## Features

- **Sync<T>**: Exclusive lock wrapper with proxy objects
- **SharedSync<T>**: Reader-writer lock wrapper
- **Pool<T>**: Thread-safe resource pool with RAII
- **Callback<T>**: Promise/future wrapper for async results
- **InterruptSignal**: stop_token-like interrupt handling

## API Overview

| Type | Description |
|------|-------------|
| `Sync<T>` | Mutex-protected value with lock() proxy |
| `SharedSync<T>` | Shared mutex with read_lock()/write_lock() |
| `Pool<T>` | Resource pool with acquire()/release() |
| `Callback<T>` | Async result callback |
| `InterruptSignal` | Thread interrupt signaling |

## Building

```bash
buck2 build //src/straylight/nix/sync:sync
buck2 test //src/straylight/nix/sync/tests:...
```

## See Also

- [Architecture](docs/ARCHITECTURE.md)
- [Testing](docs/TESTING.md)
