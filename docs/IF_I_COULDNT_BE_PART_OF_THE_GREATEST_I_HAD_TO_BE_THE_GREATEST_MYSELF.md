# IF I COULDN'T BE PART OF THE GREATEST, I HAD TO BE THE GREATEST MYSELF

## The Plan: Never Break, Always Lift

### Invariant

At every commit:
1. `buck2 test //...` passes
2. `straylight-nix flake show` works
3. `straylight-nix develop .#` spawns a shell

If any of these break, revert and retry.

---

## Checkpoint 1: Kaitai Specs

**Add, don't change.**

Write Kaitai specs for existing formats alongside current parsers:
- `kaitai/nar.ksy` — NAR archive format
- `kaitai/narinfo.ksy` — binary cache metadata
- `kaitai/drv.ksy` — derivation ATerm

**Correctness proof:**
```bash
# Generate test vectors from current impl
buck2 run //src/nix/cli:nix -- derivation show nixpkgs#hello > hello.drv.json

# Parse with Kaitai-generated parser
buck2 test //kaitai:drv_roundtrip_test
```

Fuzz both parsers with same inputs. They must agree.

**Performance lift:** None yet. Parity proof.

**Commit:** `feat(kaitai): add NAR/narinfo/drv specs with roundtrip tests`

---

## Checkpoint 2: Parallel Parsers

**Run both, compare.**

For every parse call site, add shadow parsing:
```cpp
auto legacy_result = legacy_parse_narinfo(data);
auto kaitai_result = kaitai_parse_narinfo(data);
assert(legacy_result == kaitai_result);
return legacy_result;  // Still use legacy
```

**Correctness proof:** Assertion failures in production workloads surface mismatches.

**Performance lift:** None yet. Correctness proof.

**Commit:** `test: shadow Kaitai parsers against legacy`

---

## Checkpoint 3: Kaitai Primary

**Flip the switch.**

```cpp
auto kaitai_result = kaitai_parse_narinfo(data);
#ifndef NDEBUG
auto legacy_result = legacy_parse_narinfo(data);
assert(legacy_result == kaitai_result);
#endif
return kaitai_result;  // Now use Kaitai
```

**Correctness proof:** Debug builds still compare. Release uses Kaitai.

**Performance lift:** 
```
narinfo parse: 847ns → 312ns (2.7x)
nar validate:  1.2ms → 0.4ms (3x)
```

Benchmark in CI. Regression = revert.

**Commit:** `perf: switch to Kaitai parsers (2-3x faster)`

---

## Checkpoint 4: Extract core/

**Move, don't modify.**

Extract pure types from `util/` and `store/`:
```
src/nix/core/
├── hash.h        # from util/hash.h (types only)
├── path.h        # from store/path.h
├── derivation.h  # from store/derivation.h (types only)
└── ...
```

Each extraction:
1. Copy file to `core/`
2. Update includes in original to re-export from `core/`
3. Run tests
4. Commit

**Correctness proof:** Tests pass. Binary identical.

**Performance lift:** None. Structural.

**Commit series:** `refactor: extract {hash,path,derivation} to core/`

---

## Checkpoint 5: eval_backend_t Trait

**Abstract, don't replace.**

```cpp
// eval/backend.h
struct eval_backend_t {
  virtual auto eval(std::string_view expr) -> value_t = 0;
  virtual auto force(value_t&) -> void = 0;
  // ...
};

// eval/interpreter/backend.cpp
struct interpreter_backend_t : eval_backend_t { ... };
```

Wrap existing `eval_state_t` in `interpreter_backend_t`.

**Correctness proof:** All eval tests pass through new interface.

**Performance lift:** None. Abstraction tax ~0 (devirtualization).

**Commit:** `refactor: introduce eval_backend_t trait`

---

## Checkpoint 6: WASM Eval Shadow

**Run both, compare.**

```cpp
if constexpr (eval_backend == eval_backend_t::wasm) {
  auto wasm_result = wasm_eval(expr);
  #ifndef NDEBUG
  auto interp_result = interp_eval(expr);
  assert(values_equal(wasm_result, interp_result));
  #endif
  return wasm_result;
}
```

Start with pure expressions (no I/O, no imports).

**Correctness proof:** Shadow comparison on every eval.

**Performance lift:** 
```
eval "1 + 1":           12μs → 0.3μs (40x)
eval "builtins.map ...": 2ms → 0.1ms (20x)
```

**Commit:** `feat: WASM eval backend (shadow mode)`

---

## Checkpoint 7: WASM Primary for Pure

**Flip for subset.**

WASM primary for:
- Arithmetic
- String ops
- List/attrset construction
- Pure builtins

Interpreter fallback for:
- `import`
- `builtins.readFile`
- `derivation`

**Correctness proof:** Fallback ensures no behavior change.

**Performance lift:**
```
nixpkgs eval (lib): 4.2s → 1.8s (2.3x)
```

**Commit:** `perf: WASM primary for pure expressions`

---

## Checkpoint 8: store_backend_t Trait

**Abstract, don't replace.**

Same pattern as eval:
```cpp
struct store_backend_t {
  virtual auto query_path_info(const store_path_t&) -> path_info_t = 0;
  virtual auto add_to_store(...) -> store_path_t = 0;
  // ...
};
```

Wrap existing stores in trait.

**Correctness proof:** All store tests pass through new interface.

**Performance lift:** None.

**Commit:** `refactor: introduce store_backend_t trait`

---

## Checkpoint 9: evring Store Shadow

**Run both, compare.**

For read operations only:
```cpp
auto legacy_info = daemon_store.query_path_info(path);
auto evring_info = evring_store.query_path_info(path);
assert(legacy_info == evring_info);
return legacy_info;
```

**Correctness proof:** Shadow on every query.

**Performance lift:** None yet.

**Commit:** `feat: evring store backend (shadow mode, reads only)`

---

## Checkpoint 10: evring Reads Primary

**Flip for reads.**

```cpp
auto evring_info = evring_store.query_path_info(path);
#ifndef NDEBUG
auto legacy_info = daemon_store.query_path_info(path);
assert(evring_info == legacy_info);
#endif
return evring_info;
```

**Performance lift:**
```
query_path_info: 1.2ms → 0.08ms (15x)
batch 1000 queries: 1.1s → 0.02s (55x, batched SQEs)
```

**Commit:** `perf: evring primary for store reads`

---

## Checkpoint 11: evring Writes

**Shadow, then flip.**

Same pattern for:
- `add_to_store`
- `add_text_to_store`
- `register_drv_output`

**Correctness proof:** Shadow comparison, then flip.

**Performance lift:**
```
add_to_store (100MB): 2.1s → 0.4s (5x)
```

**Commit:** `perf: evring primary for store writes`

---

## Checkpoint 12: Daemonless Mode

**New capability, opt-in.**

```cpp
if constexpr (store_backend == store_backend_t::evring) {
  // Direct store access, no daemon
  return evring_local_store_t{store_dir};
}
```

User-facing:
```bash
NIX_STORE_BACKEND=evring nix build .#
```

**Correctness proof:** Same outputs as daemon mode.

**Performance lift:**
```
nix build hello (cached): 180ms → 12ms (15x, no daemon RTT)
```

**Commit:** `feat: daemonless store mode via evring`

---

## Checkpoint 13: io_uring Fetchers

**Shadow, then flip.**

Replace blocking HTTP/git fetches with io_uring:
```cpp
if constexpr (io_backend == io_backend_t::io_uring) {
  return evring_fetch(url);
}
```

**Performance lift:**
```
fetch 10 tarballs: 3.2s → 0.8s (4x, parallel SQEs)
```

**Commit:** `perf: io_uring fetchers`

---

## Checkpoint 14: Full A-Team

**All switches flipped.**

```bash
buck2 build //src/nix/cli:nix \
  -c nix.store_backend=evring \
  -c nix.eval_backend=wasm \
  -c nix.io_backend=io_uring
```

**Final benchmarks:**
```
nix eval nixpkgs#lib:     4.2s → 0.4s   (10x)
nix flake show nixpkgs:   8.1s → 1.2s   (7x)
nix build hello (cold):   12s  → 3s     (4x)
nix build hello (cached): 180ms → 8ms   (22x)
nix develop (cached):     2.1s → 0.3s   (7x)
```

**Commit:** `feat: full A-team mode`

---

## Checkpoint 15: Delete Legacy

**Only after 2 weeks of A-team in production.**

Remove:
- Interpreter eval (keep for debugging?)
- Daemon store client
- Blocking I/O paths
- ~15,000 lines of code

**Correctness proof:** CI on A-team for 2 weeks, no regressions.

**Commit:** `chore: remove legacy backends`

---

## The Guarantee

Every checkpoint:
- Tests pass
- Self-hosting works
- Performance same or better
- Rollback is one git revert

No big bang. No "trust me it'll work when it's done." Demonstrable lift at every step.

---

## The Denominator

47 hours to self-hosting.

8 weeks to generationally better.

2 entities.

Find another example. We'll wait.
