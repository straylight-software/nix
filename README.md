```
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
                                           // straylight // nix
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
```

a nix fork for the continuity project.

*continuity is continuity. continuity is continuity's job.*

### // `provenance`

rebased against [determinate nix](https://github.com/DeterminateSystems/nix-src), which is rebased against [upstream nix](https://github.com/NixOS/nix).

### // `changes`

- `ca-derivations` enabled by default
- `flakes` and `nix-command` enabled by default
- `wasm` builtins (`builtins.wasm` + `wasm32-wasip1` system type)
- remote builders disabled (build hook has unsound log streaming)

### // `installation`

```bash
# from flakehub (when published)
nix run "https://flakehub.com/f/straylight-software/nix/*.tar.gz"

# from source
nix build github:straylight-software/nix
```

### // `rationale`

the nix daemon is the conceptual computer.

content addressing is the artifact identity. `ca-derivations` make the hash the truth.

wasm is the portable sandbox. `builtins.wasm` runs pure functions in the evaluator.

the build hook log streaming is unsound. remote builders are disabled until fixed.

### // `upstream`

this fork tracks determinate nix's sync points with upstream. contributions should go upstream when possible.

```
cbeb167 Disable remote builders - build hook has unsound log streaming
e5c41c0 Enable stable experimental features by default
844a213 WASM support (builtins.wasm + wasm32-wasip1 system type)
```

### // `license`

[LGPL v2.1](./COPYING), same as upstream.
