```
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
                                          // straylight // nix
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
```

a ground-up rethinking of nix.

### // `what`

**nix2 store** — daemonless, io_uring-native store database. no sqlite, no nix-daemon. log-structured
with lockless reads and BLAKE3 checksums. 10-25x faster than sqlite.

**libevring** — deterministic async I/O. state machines over io_uring with perfect replay for
testing. http/1, http/2, http/3 (quic via libressl).

**nix-language** — nix expression compiler targeting wasm. parse → ast → binaryen → wasm module.
`builtins.wasm` runs pure functions in the evaluator.

**nix-protocol** — formal protocol specs in kaitai struct with polyglot serializers (c++, rust,
haskell). test vectors from captured daemon traffic.

**primitives** — 34 utility modules replacing nix's NIH implementations with modern libraries.
stringzilla, ada url, blake3, re2, taskflow, rapidfuzz. 1251 test cases.

### // `build`

```bash
nix develop
buck2 build //...
buck2 test //src/straylight/...
```

### // `architecture`

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

### // `defaults`

- `ca-derivations` enabled
- `flakes` and `nix-command` enabled
- `wasm` builtins enabled
- remote builders disabled (unsound log streaming)

### // `dependencies`

libressl (not openssl), blake3, ada, re2, binaryen, wasmtime, liburing, nghttp2, ngtcp2, nghttp3.

### // `license`

[LGPL v2.1](./COPYING)
