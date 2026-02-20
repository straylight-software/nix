# Nix2 Store Design

## Overview

The Nix2 store (`straylight::nix::primitives::store`) is a complete reimplementation of the Nix
store database, designed for:

1. **Daemonless operation** - No nix-daemon required for coordination
2. **io_uring-native I/O** - Async, parallel reads via Linux io_uring
3. **Crash safety** - Log-structured with BLAKE3 checksums
4. **High throughput** - Lockless reads, bulk operations, zero-copy paths

This document contrasts the Nix2 store with:

- **Nix1 Store** - SQLite-based, daemon-coordinated
- **NativeLink CAS** - gRPC-based content-addressable storage
- **Git** - DAG-based object store with packfiles

______________________________________________________________________

## Architecture Comparison

### Storage Model

| System | Model | Files | Consistency | |--------|-------|-------|-------------| | **Nix2** | Log +
Index | Many small files | Log is source of truth | | **Nix1** | SQLite | Single db.sqlite + WAL |
SQLite ACID | | **NativeLink** | Blob + Metadata | Sharded blobs | gRPC transactions | | **Git** |
Objects + Refs | Loose objects + packfiles | Ref updates atomic |

### Key Insight: Nix2 Log-Structured Design

```
/nix/var/nix/db/
├── log/
│   └── current.log          # Append-only operation log
├── index/
│   ├── paths/{shard}/{hash}.meta     # Materialized path info
│   ├── refs/{shard}/{hash}.refs      # Forward references
│   └── referrers/{shard}/{hash}.referrers  # Reverse index
├── head                      # Current sequence number
└── lock                      # flock for write serialization
```

The log is the source of truth. The index is a materialized view that can be rebuilt from the log at
any time. This separation enables:

- **Lockless reads**: Index files are atomically renamed, no coordination needed
- **Parallel writes**: Log append is O(1), index update is independent per path
- **Instant recovery**: Replay log entries after last checkpoint

______________________________________________________________________

## Detailed Comparisons

### 1. Nix1 Store (SQLite)

#### Schema

```sql
-- Core tables
ValidPaths(id, path, hash, registrationTime, deriver, narSize, ultimate, sigs, ca)
Refs(referrer, reference)  -- Both are ValidPaths.id
DerivationOutputs(drv, id, path)
```

#### Characteristics

| Aspect | Nix1 | Nix2 | |--------|------|------| | **Database** | SQLite (single file) | Log +
sharded files | | **Read path** | SQL query + parse | Direct file read (mmap-able) | | **Write
path** | SQL transaction + WAL | Log append + atomic rename | | **Locking** | SQLite busy wait +
big-lock file | flock (kernel-managed) | | **Recovery** | SQLite WAL replay | Log replay from
checkpoint | | **Daemon** | Required for coordination | Not required | | **Bulk reads** | Sequential
SQL queries | io_uring parallel statx/read | | **Corruption detection** | SQLite integrity check |
BLAKE3 per-entry checksums |

#### Why Nix1 Needs a Daemon

SQLite is designed for single-process access. To allow multiple Nix commands to share the store:

1. `nix-daemon` holds the database connection
2. All nix commands connect via Unix socket
3. Daemon serializes write operations
4. Adds latency, complexity, and a single point of failure

#### Nix2 Daemon-Free Design

```
Process A (reading)           Process B (writing)
    │                              │
    ├── read index/paths/ab/*.meta │
    │   (lockless, direct read)    ├── flock(lock, LOCK_EX)
    │                              ├── append to log
    │                              ├── fsync(log)
    │                              ├── atomic_write(index file)
    │                              └── funlock
    │                              
    └── sees new file after rename ─┘
```

The kernel's flock is process-death safe. If a writer crashes, the kernel releases the lock. No
daemon needed to clean up.

### 2. NativeLink CAS

NativeLink is a remote execution CAS (Content-Addressable Storage) used by Bazel, Buck2, etc.

#### Architecture

```
┌─────────────────┐     ┌──────────────────┐
│   Client        │────▶│  CAS Service     │
│   (gRPC)        │     │  (FindMissing,   │
└─────────────────┘     │   BatchRead,     │
                        │   BatchUpdate)   │
                        └────────┬─────────┘
                                 │
                        ┌────────▼─────────┐
                        │  Storage Backend │
                        │  (S3, GCS, disk) │
                        └──────────────────┘
```

#### Comparison

| Aspect | NativeLink CAS | Nix2 Store | |--------|----------------|------------| | **Protocol** |
Remote Execution API (gRPC) | Local filesystem | | **Addressing** | Content hash (SHA-256) | Store
path (hash-name) | | **Deduplication** | By content hash | By store path hash | | **References** |
ActionResult has dependencies | Refs index | | **Network** | First-class (remote storage) | Local
only (remote via http) | | **Use case** | Build farm, CI | Single-machine or cluster |

#### Key Differences

1. **Content vs Path Addressing**

   - NativeLink: `sha256:abc123` → blob
   - Nix2: `/nix/store/abc123-foo` → path_info + NAR

2. **Dependency Model**

   - NativeLink: ActionResult embeds dependency hashes
   - Nix2: Explicit Refs table with forward + reverse index

3. **Reference Scanning**

   - NativeLink: Trust client's declared dependencies
   - Nix2: NAR scanning for embedded store paths

### 3. Git

Git is a DAG-based version control system with an object store.

#### Object Model

```
blob   = raw file content
tree   = directory listing (mode, name, hash)
commit = tree + parent(s) + metadata
tag    = annotated pointer to commit
```

#### Storage

```
.git/objects/
├── ab/cdef123...   # Loose object (zlib compressed)
├── pack/
│   ├── pack-xxx.idx  # Pack index
│   └── pack-xxx.pack # Delta-compressed objects
└── info/
```

#### Comparison

| Aspect | Git | Nix2 Store | |--------|-----|------------| | **Object identity** | SHA-1/SHA-256 of
content | Path = f(drv hash, name) | | **References** | Embedded in tree/commit | Separate Refs
index | | **Compression** | zlib + delta encoding | NAR (uncompressed store) | | **History** | Full
commit DAG | Registration time only | | **GC** | Reachability from refs | Reachability from roots |
| **Packfiles** | Space optimization | No equivalent (NAR dedup) |

#### Key Differences

1. **Mutability**

   - Git: refs (branches) are mutable pointers
   - Nix2: Paths are immutable once registered

2. **Delta Encoding**

   - Git: Packfiles use delta compression
   - Nix2: No cross-path compression (could use ZSTD for NAR)

3. **Object Size**

   - Git: Optimized for source code (small files)
   - Nix2: Optimized for packages (large directory trees)

______________________________________________________________________

## Nix2 Store Internals

### Data Structures

#### path_info (ValidPathInfo equivalent)

```cpp
struct path_info {
  std::string path;               // /nix/store/hash-name
  std::string nar_hash;           // base16 SHA-256 of NAR
  std::int64_t registration_time; // Unix timestamp
  std::string deriver;            // Derivation that built this
  std::int64_t nar_size;          // NAR size in bytes
  bool ultimate;                  // Built locally (trusted)
  std::vector<std::string> sigs;  // Signatures
  std::string ca;                 // Content-address assertion
};
```

#### log_entry

```cpp
struct log_entry {
  log_op op;                      // register_path, invalidate_path, etc.
  std::uint64_t sequence;         // Monotonic sequence number
  std::int64_t timestamp;         // Unix timestamp
  std::vector<std::byte> data;    // Payload (zpp_bits serialized)
};
```

### Write Path

```
register_path(info, refs)
    │
    ├── 1. flock(lock, LOCK_EX)
    │
    ├── 2. Serialize payload
    │       log_entry {
    │         op: register_path,
    │         seq: current_seq + 1,
    │         data: serialize(info, refs)
    │       }
    │
    ├── 3. Compute BLAKE3 checksum
    │
    ├── 4. Append to log: [len][data][checksum]
    │
    ├── 5. fsync(log)
    │       fsync(log_dir)  # crash safety
    │
    ├── 6. Update index files (atomic rename):
    │       - paths/{shard}/{hash}.meta
    │       - refs/{shard}/{hash}.refs
    │       - referrers/{ref_hash}.referrers (for each ref)
    │
    ├── 7. Update head sequence
    │
    └── 8. funlock(lock)
```

### Read Path (Lockless)

```
query_path_info(store_path)
    │
    ├── 1. Extract hash from path
    │       /nix/store/abc123-foo → abc123
    │
    ├── 2. Compute shard (first 2 chars)
    │       abc123 → ab
    │
    ├── 3. Read index file (direct or io_uring)
    │       index/paths/ab/abc123.meta
    │
    └── 4. Deserialize and return
```

### Bulk Operations (io_uring)

```
bulk_query_path_info([path1, path2, ..., pathN])
    │
    ├── 1. Compute all index file paths
    │
    ├── 2. Submit batched io_uring operations:
    │       - openat (parallel)
    │       - read (pipelined)
    │       - close (cleanup)
    │
    ├── 3. Process completions as they arrive
    │
    └── 4. Return results vector
```

### Recovery

```
recover()
    │
    ├── 1. Read log from sequence 0
    │       - Verify BLAKE3 checksum per entry
    │       - Stop at first corrupt entry
    │
    ├── 2. For each log entry:
    │       - replay_entry() rebuilds index
    │
    └── 3. Index now consistent with log
```

### Compaction

```
compact()
    │
    ├── 1. flock(lock, LOCK_EX)
    │
    ├── 2. Find max sequence in log
    │
    ├── 3. Delete old log
    │
    ├── 4. Write checkpoint entry:
    │       log_entry {
    │         op: checkpoint,
    │         seq: max_seq + 1
    │       }
    │
    ├── 5. Index is now authoritative
    │       (log only needed for new writes)
    │
    └── 6. funlock(lock)
```

______________________________________________________________________

## Performance Characteristics

### Benchmarks (Theoretical)

| Operation | Nix1 (SQLite) | Nix2 | Speedup | |-----------|---------------|------|---------| |
Single path lookup | ~50μs | ~5μs | 10x | | Bulk 1000 paths | ~50ms (sequential) | ~2ms (parallel
io_uring) | 25x | | Check 10K validity | ~1s | ~50ms (statx batched) | 20x | | Reference closure |
O(n) queries | O(log n) BFS rounds | variable | | Register path | ~1ms | ~200μs | 5x |

### Why Nix2 is Faster

1. **No SQL parsing**: Direct binary read vs SQL compilation
2. **No daemon roundtrip**: Local file access vs socket IPC
3. **io_uring batching**: 256+ operations in flight
4. **Sharding**: 256 directories = reduced contention
5. **Lockless reads**: No reader-writer coordination

______________________________________________________________________

## Crash Safety Analysis

### Failure Modes

| Crash Point | Nix1 | Nix2 | |-------------|------|------| | During log write | SQLite WAL rollback
| Entry ignored (checksum fails) | | After log, before index | SQLite consistent | Index rebuilt
from log | | During index update | SQLite consistent | Old index still valid | | Corrupted storage |
SQLite integrity_check | BLAKE3 detects on read |

### Guarantees

1. **Durability**: Log entry survives crash iff fsync completed
2. **Atomicity**: Index file visible iff rename completed
3. **Consistency**: Index always matches some log prefix
4. **Integrity**: BLAKE3 checksum detects bit rot

______________________________________________________________________

## Future Enhancements

### 1. Registered Buffers (io_uring)

Pre-register read buffers with the kernel for zero-copy I/O.

### 2. ZSTD Compression

Compress .meta files for reduced I/O and storage.

### 3. Memory-Mapped Index

mmap index files for kernel page cache management.

### 4. Derivation Reverse Index

O(1) drv_path → output_path lookups (currently O(n) scan).

### 5. Remote Log Replication

Stream log entries to remote store for clustering.

### 6. Content-Addressed Deduplication

Like Git packfiles but for NAR content.

______________________________________________________________________

## Migration Path from Nix1

```
nix2-migrate
    │
    ├── 1. Read all rows from ValidPaths
    │
    ├── 2. For each path:
    │       - read references from Refs table
    │       - call nix2::register_path(info, refs)
    │
    ├── 3. Read DerivationOutputs
    │       - call nix2::add_derivation_output()
    │
    ├── 4. Checkpoint
    │
    └── 5. Verify: nix2::verify() → true
```

Estimated migration time for 100K paths: ~30 seconds.

______________________________________________________________________

## Summary

| Feature | Nix1 | Nix2 | NativeLink | Git | |---------|------|------|------------|-----| | Local
perf | Fair | Excellent | N/A | Good | | Daemon-free | No | Yes | N/A | Yes | | Crash safe | Yes
(SQLite) | Yes (log+checksum) | Yes (backend) | Yes | | Bulk ops | Poor | Excellent | Good (gRPC) |
Good (packfile) | | Remote support | HTTP | HTTP | Native | Native | | Content dedup | NAR hash |
NAR hash | Content hash | Delta + pack | | Reference model | FK + triggers | Flat files |
ActionResult | Tree embedding |

The Nix2 store is purpose-built for the Nix use case: immutable paths with explicit references,
optimized for local high-throughput operations while maintaining crash safety through a
log-structured design.
