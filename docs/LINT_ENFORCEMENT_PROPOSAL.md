# lint enforcement parity: design doc

**Status: PARTIALLY IMPLEMENTED** (see notes below)

## problem statement

the codebase has excellent lint tooling and comprehensive style documentation, but there's a gap
between what's _documented_ and what's _mechanically enforced_. conventions exist that rely on human
discipline rather than CI gates. when a convention isn't enforced by tooling, it will eventually be
violated — especially in an agent-heavy workflow.

recent example: the iostring/sstream ban. we've decided that `<sstream>` and `std::ostringstream`
are banned (performance, allocation patterns), but there's no ast-grep rule or clang-tidy check
blocking it. result: 434 matches still exist in the codebase, and new code can introduce more.

## current state

### tools available

| tool | config | integration | enforcement | |------|--------|-------------|-------------| |
clang-format | `.clang-format` | `nix fmt`, treefmt | `nix flake check` | | clang-tidy |
`.clang-tidy` | `scripts/lint` | pre-commit (push) | | ast-grep | `sgconfig.yml`, `rules/` (22
rules) | `ast-grep scan` | `nix flake check`, pre-commit | | cppcheck | `.cppcheck`, `cppcheck.cfg`
| available in devshell | manual only | | nixfmt/deadnix/statix | treefmt | `nix fmt` |
`nix flake check` |

### what's enforced automatically

1. **`nix fmt`** - runs all treefmt formatters (clang-format, nixfmt, shfmt, etc.)
2. **`nix flake check`** - verifies formatting is applied (treefmt.build.check)

### what's NOT enforced automatically

1. **clang-tidy** - semantic lint rules (naming, complexity, bugs)
2. **ast-grep** - pattern rules (no-class-keyword, no-sstream, etc.)
3. **cppcheck** - deep static analysis
4. **pre-commit hooks** - documented but no `.pre-commit-config.yaml` exists

### documented conventions without mechanical enforcement

| convention | documented in | enforcement | |------------|--------------|-------------| | no
`<sstream>` | (recent decision) | none | | no `class` keyword | cpp-style-guide.md | ast-grep rule
exists (warning) | | three-letter rule | cpp-style-guide.md | partial ast-grep (10 specific names) |
| dangerous names | cpp-style-guide.md | none | | `_t` suffix for types | cpp-style-guide.md |
clang-tidy (only runs manually) | | trailing return types | cpp-style-guide.md | clang-tidy +
ast-grep (manual) | | struct-only exception | cpp-style-guide.md | none (can't distinguish valid
uses) |

## proposal: perfect parity

### principle

**every documented convention must have mechanical enforcement. if it can't be enforced, it should
be documented as advisory-only with explicit rationale.**

### phase 1: enforce existing rules in CI

add a new nix flake check that runs our lint tools:

```nix
checks = {
  formatting = config.treefmt.build.check inputs.self;

  # NEW: semantic lint checks
  lint-cpp = pkgs.runCommand "lint-cpp" {
    nativeBuildInputs = [ toolchain.llvm.clang-tools pkgs.ast-grep ];
    src = ./.;
  } ''
    cd $src

    # ast-grep: pattern rules (errors only, not warnings/hints)
    ast-grep scan --config sgconfig.yml --json src/ | \
      jq -e 'map(select(.severity == "error")) | length == 0' || \
      { echo "ast-grep found errors"; exit 1; }

    # clang-tidy: semantic lint on changed files or all src/straylight/
    # (full codebase tidy is expensive, start with straylight/)
    find src/straylight -name '*.cpp' -exec clang-tidy {} \; || \
      { echo "clang-tidy violations found"; exit 1; }

    touch $out
  '';
};
```

### phase 2: add missing rules

#### 2a. ban sstream/ostringstream/istringstream [IMPLEMENTED]

`rules/no-sstream.yml` exists:

```yaml
id: no-sstream
language: cpp
severity: error
message: "std::ostringstream/istringstream banned - use fmt::format or string concatenation"
rule:
  any:
    - pattern: std::ostringstream $VAR
    - pattern: std::istringstream $VAR
    - pattern: std::stringstream $VAR
note: |
  sstream is slow (allocations, type erasure). alternatives:
  - fmt::format("{}", value) for string building
  - std::to_string() for simple conversions
  - std::from_chars() for parsing
```

also add to `.clang-tidy` header filter or create custom check.

#### 2b. ban dangerous generic names as members [IMPLEMENTED]

`rules/no-dangerous-member-names.yml` exists:

```yaml
id: no-dangerous-member-names
language: cpp
severity: warning
message: "Generic member name '$NAME' - use more specific name (e.g., content_hash_, cached_value_)"
rule:
  any:
    - pattern: $TYPE hash_;
    - pattern: $TYPE value_;
    - pattern: $TYPE state_;
    - pattern: $TYPE path_;
    - pattern: $TYPE mutex_;
```

#### 2c. expand three-letter rule

the current `no-short-identifier.yml` only checks 10 specific abbreviations. expand to catch more:

- `mgr` → `manager`
- `ptr` → `pointer`
- `buf` → `buffer`
- `tmp` → `temporary`
- `str` → `string`
- `err` → `error`

### phase 3: pre-commit hooks [IMPLEMENTED]

`.pre-commit-config.yaml` exists (generated from `dhall/pre-commit.dhall`):

```yaml
repos:
  - repo: local
    hooks:
      - id: nix-fmt
        name: nix fmt
        entry: nix fmt -- --fail-on-change
        language: system
        pass_filenames: false

      - id: ast-grep-errors
        name: ast-grep (errors)
        entry: ast-grep scan --config sgconfig.yml --filter 'severity == "error"'
        language: system
        types: [c++]

      - id: clang-tidy
        name: clang-tidy
        entry: scripts/lint
        language: system
        types: [c++]
```

### phase 4: editor integration

ensure `.clangd` config enables all relevant warnings so developers see issues inline:

```yaml
# .clangd
CompileFlags:
  Add:
    - -Weverything
    - -Wno-c++98-compat
    - -Wno-c++98-compat-pedantic
    - -Wno-padded

Diagnostics:
  ClangTidy:
    Add: ["*"]
    Remove: ["abseil-*", "altera-*", "android-*", ...]
```

### phase 5: fix existing violations

once enforcement is in place, systematic cleanup:

1. **sstream removal** - 434 violations, likely ~50 unique patterns to fix
2. **naming violations** - ongoing via `scripts/fix-*.py`
3. **struct-only violations** - review existing `class` uses for legitimacy

## severity model

| severity | meaning | enforcement | |----------|---------|-------------| | **error** | hard block,
cannot merge | CI fails | | **warning** | should fix, tracked | CI warns, accumulates tech debt | |
**hint** | style preference | editor only, no CI |

current ast-grep rules use mixed severities. proposal: promote core conventions to error:

- `no-class-keyword` → error (already error)
- `no-using-namespace-file-scope` → error (already error)
- `no-sstream` → error (new)
- `trailing-return-type` → warning → error (promotion)

## configuration inconsistency fix [IMPLEMENTED]

~~current issue: `.editorconfig` says 4-space C++ indent, `.clang-format` says 2-space.~~

Fixed: `.editorconfig` now uses 2-space indent for C++ to match `.clang-format`.

## open questions

1. **clang-tidy on full codebase?** running clang-tidy on all of `src/nix/` is expensive and will
   find thousands of violations in upstream code. options:

   - only lint `src/straylight/` (our code)
   - only lint changed files (incremental)
   - establish baseline, only fail on new violations

2. **vendor exclusions?** `vendor/` is excluded from treefmt. should it be excluded from all lint?
   (probably yes)

3. **test file exclusions?** some test files intentionally violate rules (e.g., testing error
   handling). need explicit exclusion patterns.

4. **class exception validation?** the documented exception for `class` with private members for
   safety-critical types can't be mechanically validated (requires understanding intent). keep as
   documented advisory?

## implementation order

1. fix `.editorconfig` indent inconsistency (trivial)
2. add `no-sstream.yml` ast-grep rule
3. add ast-grep to `nix flake check`
4. create `.pre-commit-config.yaml`
5. add clang-tidy to CI (straylight/ only initially)
6. expand ast-grep rules for remaining conventions
7. systematic violation cleanup

## success criteria

- `nix flake check` fails on any:
  - formatting violations (already works)
  - ast-grep errors (new)
  - clang-tidy errors in src/straylight/ (new)
- pre-commit hooks catch issues before commit
- editor integration shows issues inline
- documented conventions == enforced conventions (with explicit advisory-only exceptions)

## appendix: complete rule inventory

### ast-grep rules (current 22)

| rule | severity | status | |------|----------|--------| | no-class-keyword | error | enforced | |
no-using-namespace | error | enforced | | no-c-style-cast | warning | tracks | | no-raw-new |
warning | tracks | | no-std-endl | warning | tracks | | no-typedef | warning | tracks | |
no-short-identifier-\* (10) | warning | tracks | | no-assert | warning | tracks | |
no-magic-numbers-\* | hint | editor | | prefer-nullptr | warning | tracks | | prefer-string-view |
hint | editor | | prefer-span-\* | warning | tracks | | trailing-return-type | warning | tracks | |
uppercase-literal-suffix | warning | tracks | | aaa-make-shared | hint | editor | | aaa-make-unique
| hint | editor | | aaa-static-cast | hint | editor | | no-implicit-bool-conversion | hint | editor
| | no-nodiscard-missing | hint | editor |

### proposed new rules

| rule | severity | purpose | |------|----------|---------| | no-sstream | error | ban
ostringstream/istringstream | | no-dangerous-member-names | warning | ban generic names as members |
| no-short-identifier-expanded | warning | more three-letter abbreviations |

### clang-tidy enforcement

current: manual via `scripts/lint` proposed: CI on `src/straylight/`, incremental on changed files
