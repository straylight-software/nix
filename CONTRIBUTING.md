# ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

# // straylight // nix // contributing

# ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

# Contributing to straylight/nix

This is a fork of Nix with experimental components for deterministic async I/O, Nix-to-WASM
compilation, and formal protocol specifications.

## // build

```bash
nix develop
buck2 build //...
buck2 test //src/straylight/...
```

The build system is Buck2 via sensenet.

n.b. The `doc/manual/` directory contains upstream Nix documentation which references Meson. That
applies to the upstream project; straylight components (`src/straylight/`) build with Buck2 only.

## // architecture

```
src/
├── nix/                      # core nix fork (C++23)
├── nix-c/                    # C API bindings
└── straylight/
    ├── evring/               # deterministic async I/O (io_uring)
    ├── language/             # nix → wasm compiler
    ├── nix/primitives/       # modern utility replacements
    └── protocol/             # formal protocol specs (kaitai)
```

## // style

See `docs/cpp-style-guide.md` for C++ conventions.

For typographical conventions (comment style, delimiters, epigraphs), see
https://github.com/straylight-software — these are organization-wide standards.

Key points:

- `snake_case` for everything
- `_t` suffix for types: `store_path_t`, `hash_type_t`
- trailing underscore for private members: `path_`, `cache_`
- lowercase comments for working notes
- proper capitalization for documentation comments

## // tests

```bash
# run all straylight tests
buck2 test //src/straylight/...

# run specific test target
buck2 test //src/straylight/language/tests:execution_test
```

## // commits

Follow conventional commit style:

- `fix:` bug fixes
- `feat:` new features
- `refactor:` code changes that neither fix bugs nor add features
- `docs:` documentation only
- `test:` adding or updating tests
- `chore:` maintenance tasks

## // pull requests

1. Fork and create a branch
2. Make changes with tests
3. Ensure `buck2 build //...` passes
4. Submit PR with clear description

## // license

LGPL-2.1. See `COPYING`.
