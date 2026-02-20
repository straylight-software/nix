# straylight nix c++ style guide

## for agentic identities

**this document has two tiers:**

- **tier 1 (first 8kb):** naming, structure, basic patterns — sufficient for single-file
  modifications, style fixes, mechanical refactoring. junior agents doing point edits stop
  at the tier boundary marker.

- **tier 2 (full 15kb):** includes c++23 patterns, error handling, threading, performance,
  architecture. required for design decisions, new types, API changes.

if you are making design decisions, read the entire document. if you cannot fit 15kb in context,
you are not ready for design work in this codebase.

### rules exist to serve intentions

style rules encode the *intentions* of the codebase maintainers. when applying a rule would
violate the intention it was meant to serve, you must recognize this and act accordingly:

1. **stop and reason about consequences** — before mechanical application of any rule, ask:
   "what was this rule designed to prevent? would applying it here cause that very harm?"

2. **the stable_ref precedent** — the rule "use struct, all members public" exists to reduce
   cognitive overhead and eliminate hidden state. but `stable_ref<T>` uses private constructors
   specifically to *prevent* hidden state corruption (dangling pointers from copied buffers).
   applying the rule mechanically would have destroyed the safety invariant it was meant to protect.
   the correct action was to document an exception, not to blindly convert.

3. **when you discover a conflict, document it** — if a rule's mechanical application would
   violate its own purpose, add an exception clause to this guide explaining:
   - what the conflict is
   - why the exception exists
   - how to recognize similar cases

4. **the maintainer deeply approves of this behavior** — exercising judgment about intentions
   vs. mechanical rule-following is not insubordination. it is the entire point. agents that
   recognize "this rule would break things here" and stop to document why are doing exactly
   what is wanted. agents that barrel through and break things while following rules to the
   letter are failing at their actual job.

### before you begin

- run `buck2 build //...` and verify it passes before making changes
- run it again after changes — if it fails, fix it before proceeding
- the build passing is not optional. a "style fix" that breaks the build is not a fix.

---

this is the definitive style guide for c++ code in the straylight/nix codebase. it is derived from
the straylight c++ guidelines but adapted for the nix codebase's specific needs.

## core principles

1. **optimize for disambiguation, not brevity** - agents write code in seconds, humans debug it for hours
2. **name for grep** - every identifier must be globally searchable
3. **explicit over implicit** - type annotations, full names, clear state machines
4. **modern c++23** - use the latest features, they exist to help

## naming conventions

### the fundamental rules

- **snake_case** for everything: classes, functions, variables, parameters, namespaces
- **_t suffix** for type names: `store_path_t`, `eval_state_t`, `hash_type_t`
- **trailing underscore** for member variables: `path_`, `state_`, `cache_`
- **UPPER_CASE** for macros only
- **CamelCase** only for template parameters

```cpp
// good
struct local_store_t {
  std::filesystem::path store_dir_;
  sync_t<path_cache_t> cache_;

  auto query_path_info(const store_path_t& path) -> std::optional<path_info_t>;
};

// bad - mixed conventions
class LocalStore {
  std::filesystem::path storeDir;  // missing trailing underscore
  Sync<PathCache> cache;           // wrong case

  auto queryPathInfo(const StorePath& path) -> std::optional<PathInfo>;
};
```

n.b. we use `struct` exclusively — `class` is banned. all members are public.
the trailing underscore distinguishes member variables from parameters and locals.

**exception: safety-critical encapsulation.** types that use private constructors or members
to enforce compile-time safety invariants may use `class` with private members. the canonical
example is `stable_ref<T>` / `stable_span<T>` in evring — these types intentionally restrict
construction to `make_stable_ref()` / `make_stable_span()` or `machine_storage` to guarantee
buffer lifetime safety. without private constructors, users could accidentally create dangling
references. when using this exception:

1. document why the encapsulation is safety-critical (not just "good practice")
2. prefer factory functions (`make_*`) over public constructors
3. keep the private section minimal — only what's needed for the invariant

### the three-letter rule

abbreviations under 4 characters are too short:

```cpp
// bad
auto cfg = load_cfg();
auto conn = db.get_conn();
auto res = process(req);

// good
auto configuration = load_configuration();
auto connection = database.get_connection();
auto result = process_request(request);
```

### standard abbreviations (use sparingly)

- `idx/jdx` - index (prefer descriptive names like `path_index`)
- `ctx` - context (only when type makes it unambiguous)
- `fd` - file descriptor (unix convention)
- `pid` - process id (unix convention)

### dangerous names to avoid

these names cause collisions when used as member variables because they match common
function/type names. choose more specific names:

```cpp
// dangerous - will collide with functions or types
private:
  hash_t hash;       // bad - "hash" is also a function
  value_t value;     // bad - "value" is everywhere
  state_t state;     // bad - "state" is ambiguous
  path_t path;       // bad - "path" is a type name
  std::mutex mutex;  // bad - can't add trailing underscore to std type

// better - specific and unambiguous
private:
  hash_t content_hash_;
  value_t cached_value_;
  parser_state_t parser_state_;
  std::filesystem::path store_path_;
  std::mutex cache_mutex_;
```

## code organization

### file structure

```
src/nix/
├── util/           # foundation utilities
│   ├── error.h
│   ├── error.cpp
│   ├── hash.h
│   └── hash.cpp
├── store/          # store abstraction and implementations
│   ├── store-api.h
│   ├── store-api.cpp
│   ├── local-store.h
│   └── local-store.cpp
├── expr/           # nix expression evaluator
│   ├── eval.h
│   ├── eval.cpp
│   └── nixexpr.h
├── fetchers/       # content fetchers
├── flake/          # flake support
└── cli/            # command-line interface
```

- headers and implementations live adjacent
- test files live in `tests/` subdirectories
- cuda code uses `.cu` extension (if applicable)

### headers

```cpp
#pragma once

#include <chrono>
#include <memory>
#include <span>

#include "nix/util/error.h"
#include "nix/store/path.h"

namespace nix {

class local_store_t : public store_t {
public:
  local_store_t(const store_params_t& params);

  auto query_path_info(const store_path_t& path)
    -> std::shared_ptr<const valid_path_info_t> override;

  auto add_to_store(source_t& source, std::string_view name)
    -> store_path_t override;

private:
  std::filesystem::path real_store_dir_;
  std::filesystem::path state_dir_;
  sync_t<path_info_cache_t> path_info_cache_;
};

}  // namespace nix
```

### implementation

```cpp
#include "nix/store/local-store.h"

#include <format>

#include "nix/util/logging.h"
#include "nix/store/gc.h"

namespace nix {

auto local_store_t::query_path_info(const store_path_t& path)
  -> std::shared_ptr<const valid_path_info_t> {

  auto cache_result = path_info_cache_.lock()->find(path);
  if (cache_result != path_info_cache_.lock()->end()) {
    return cache_result->second;
  }

  auto path_info = query_path_info_uncached(path);
  if (!path_info) {
    return nullptr;
  }

  path_info_cache_.lock()->insert({path, path_info});
  return path_info;
}

}  // namespace nix
```

---

## tier boundary: 8kb

**junior agents stop here.** the preceding ~8kb covers naming, structure, and basic patterns —
sufficient for single-file modifications like renaming variables, fixing style violations, or
adding trailing underscores to member variables.

**continue reading if you are:**
- designing new types or modules
- making architectural decisions
- modifying public APIs
- working on error handling, threading, or performance

if you are unsure which tier applies to your task, read the whole document.

---

## modern c++23 patterns

### error handling

we use result types for recoverable errors and exceptions for unrecoverable ones:

```cpp
// recoverable error - return result type
auto parse_store_uri(std::string_view uri)
  -> std::expected<store_uri_t, error_t> {

  if (uri.empty()) {
    return std::unexpected(error_t{"empty store URI"});
  }

  // parse...
  return store_uri_t{...};
}

// unrecoverable error - throw
if (!critical_file.exists()) {
  throw sys_error_t("critical file missing: {}", critical_file.path());
}
```

### const-correctness

```cpp
// mark everything const that can be
auto get_path_info(const store_path_t& path) const -> path_info_t;

// use const for local variables that don't change
const auto configuration = load_configuration();
const auto path_count = paths.size();

// don't forget const on methods that don't modify state
auto get_store_dir() const -> std::filesystem::path;  // const!
```

### span usage

```cpp
// use span for non-owning array views
auto hash_strings(std::span<const std::string> strings) -> hash_t;

// not raw pointer + size
auto hash_strings(const std::string* strings, size_t count) -> hash_t;  // bad
```

### trailing return types

prefer trailing return types for consistency and template readability:

```cpp
// good - consistent style
auto query_path_info(const store_path_t& path) -> path_info_t;
auto compute_hash(std::span<const std::byte> data) -> hash_t;

// acceptable for simple getters
std::string_view name() const { return name_; }
```

## agent-human collaboration

### the comment convention

- agents: properly capitalized comments
- humans: lowercase comments (straylight tradition)

```cpp
// This is agent-generated code with standard patterns
auto store = open_store(store_uri);

// human intuition: special handling needed for legacy store format
if (store->get_protocol_version() < MIN_PROTOCOL_VERSION) {
  apply_legacy_compatibility_layer(store);
}
```

### critical path marking

identify code requiring human review:

```cpp
// CRITICAL PATH: Store path validation - human review required
// Any bug here corrupts the entire store
auto validate_store_path(std::string_view path) -> bool {
  // human-written validation with aggressive checks
}

// AUXILIARY: Logging utilities - agent generation acceptable
auto format_log_message(level_t level, std::string_view msg) -> std::string;
```

## clang-tidy compliance

the codebase uses strict clang-tidy checking. key rules:

### required

- `readability-identifier-naming` - snake_case everything
- `readability-braces-around-statements` - always use braces
- `modernize-use-trailing-return-type` - prefer trailing returns
- `modernize-use-using` - use `using` instead of `typedef`
- `modernize-use-nodiscard` - mark functions that should be checked
- `performance-*` - all performance checks enabled

### configuration

see `.clang-tidy` in repository root for full configuration.

### fixing violations

when fixing clang-tidy violations:

1. **bulk fixes first** - use clang-tidy --fix for mechanical changes
2. **semantic fixes manually** - member renames require careful review
3. **test after each batch** - build and run tests frequently
4. **commit incrementally** - one type of fix per commit

## testing

### the five-minute rule

if you can't understand what code does in 5 minutes, rewrite it with better structure.

### test naming

```cpp
// test file names
store_path_test.cpp
hash_test.cpp
eval_test.cpp

// test case names - descriptive
TEST_CASE("store_path_t rejects paths with invalid characters") { ... }
TEST_CASE("hash_t produces consistent results for identical input") { ... }
```

### property-based testing

for invariants that must always hold:

```cpp
TEST_CASE("serialization roundtrip preserves store paths") {
  check_property([](const store_path_t& path) {
    auto serialized = serialize(path);
    auto deserialized = deserialize(serialized);
    return deserialized == path;
  });
}
```

## debugging patterns

### the grep test

every function should be globally unique and searchable:

```bash
# bad - too many results
grep -r "query(" .          # 500 matches

# good - finds exactly what you need
grep -r "query_path_info(" .     # 3 relevant matches
```

### state machine clarity

make states explicit:

```cpp
// bad - implicit state machine
if (flags & 0x04 && !error_flag && counter > threshold) {
  // what state is this?
}

// good - self-documenting states
enum class connection_state_t {
  disconnected,
  connecting,
  authenticated,
  active,
  draining
};

if (current_state_ == connection_state_t::authenticated &&
    error_count_ == 0 &&
    retry_count_ > max_retries_) {
  transition_to(connection_state_t::draining);
}
```

## performance guidelines

1. **start with clear, simple code** - the compiler optimizes clarity
2. **measure with production flags**: `-O3 -march=native`
3. **profile before optimizing** - data always surprises
4. **small types by value** - pass small types by value, not reference

```cpp
// let the compiler work
for (const auto& path : paths) {
  process_path(path);
}

// not this cleverness
for (auto idx = 0; idx < paths.size(); idx += 4) {
  // unrolled loop that's probably slower
}
```

## anti-patterns to avoid

### the abbreviation cascade

```cpp
// starts innocent...
auto cfg = load_config();

// spreads like a virus...
auto conn = create_conn(cfg);
auto mgr = conn_mgr(conn);
auto proc = mgr.get_proc();

// ends in debugging hell
if (!proc.is_valid()) {  // what is proc again?
  // ...
}
```

### context-dependent names

```cpp
// bad - "path" means different things
namespace store {
  class path;  // store path
}
namespace fs {
  class path;  // filesystem path
}

// good - names carry their domain
namespace store {
  class store_path_t;
}
namespace fs {
  using path_t = std::filesystem::path;
}
```

### implicit state machines

```cpp
// bad - state spread across booleans
bool is_connected;
bool is_authenticated;
bool has_error;

// good - explicit state
enum class session_state_t {
  disconnected,
  connected,
  authenticated,
  error
};
session_state_t current_state_;
```

## summary

in an agent-heavy codebase:

1. **every name must be globally unambiguous**
2. **every abbreviation creates exponential confusion**
3. **every implicit assumption becomes a debugging nightmare**
4. **every clang-tidy violation hides real issues**

write code as if 100 agents will be pattern-matching against it tomorrow, and a tired human will be
debugging it at 3am next month. because both will happen.
