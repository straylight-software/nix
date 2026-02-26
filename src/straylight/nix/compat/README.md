# straylight::nix::compat

Compatibility wrappers for external formats and tools.

## Quick Start

```cpp
#include <straylight/nix/compat/sqlite.h>
#include <straylight/nix/compat/git.h>

using namespace straylight::nix::compat;

// SQLite with RAII
auto db = Database::open("store.sqlite", OpenMode::ReadWrite);
if (!db) { /* handle error */ }

auto stmt = db->prepare("SELECT * FROM paths WHERE path = ?");
stmt->bind(1, "/nix/store/...");

for (auto row : stmt->execute()) {
    auto path = row.get<std::string>(0);
    auto hash = row.get<std::string>(1);
}

// Transaction
{
    auto txn = Transaction::begin(*db);
    db->exec("INSERT INTO ...");
    txn.commit();
}  // Rolls back if commit not called

// Git object parsing
auto obj = git::parse_object(data);
if (auto* tree = std::get_if<git::tree>(&obj)) {
    for (auto& entry : tree->entries) {
        // ...
    }
}
```

## Features

- **SQLite RAII**: Database, Statement, Transaction, Column classes
- **std::expected errors**: No exceptions, clean error handling
- **Range-based iteration**: Iterate result sets with for loops
- **Git objects**: Parse tree, blob, commit objects

## API Overview

### SQLite

| Type | Description | |------|-------------| | `Database` | RAII sqlite3\* handle | | `Statement` |
Prepared statement with bind/execute | | `Transaction` | RAII transaction with commit/rollback | |
`Row` | Result row with typed column access |

### Git

| Type | Description | |------|-------------| | `git::tree` | Git tree object | | `git::blob` | Git
blob object | | `git::commit` | Git commit object | | `parse_object()` | Parse git object from bytes
|

## Building

```bash
buck2 build //src/straylight/nix/compat:compat
buck2 test //src/straylight/nix/compat/tests:...
```

## See Also

- [Architecture](docs/ARCHITECTURE.md)
- [Testing](docs/TESTING.md)
