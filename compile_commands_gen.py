#!/usr/bin/env python3
"""Generate compile_commands.json for straylight/nix"""

import json
import os
from pathlib import Path

# Include paths based on the new structure
INCLUDES = [
    "-Isrc/nix",
    "-Isrc/nix/util",
    "-Isrc/nix/store", 
    "-Isrc/nix/expr",
    "-Isrc/nix/fetchers",
    "-Isrc/nix/flake",
    "-Isrc/nix/main",
    "-Isrc/nix/cmd",
    "-Isrc/nix/cli",
]

# System includes (from nix develop)
SYS_INCLUDES = [
    "-isystem/nix/store/2iqpqx03bxg16hn6d4k6w7dci4hgllph-boost-1.87.0-dev/include",
    "-isystem/nix/store/mwlmrp65kqqrp8b25nshm6qyqxb70pf7-nlohmann_json-3.12.0/include",
    "-isystem/nix/store/5g76svmwc0f7cbz90xdhq6r3s62cszqp-openssl-3.5.0-dev/include",
    "-isystem/nix/store/76za4vjf98fxkf06hj4bwzfxv8cn1bbi-libarchive-3.8.0-dev/include",
    "-isystem/nix/store/mbr23m9fmpcrn3sxz8r22fk1vnpbbkhz-sqlite-3.50.1-dev/include",
    "-isystem/nix/store/q9hk8gsh0wlz2yxr9pf59hn3b04wmjp7-curl-8.15.0-dev/include",
    "-isystem/nix/store/38yj3dfdi9z9r1hy7v35znl3nfh82ggs-libgit2-1.9.1/include",
    "-isystem/nix/store/pzrvvrvs2ynfvz8ycm3dw02a0lbj0ylz-brotli-1.1.0-dev/include",
    "-isystem/nix/store/7d2vmwh8cqbzgicpzznmqrn9w4ys4v7b-editline-1.17.2-dev/include",
    "-isystem/nix/store/q1bkd0cna7jb1ixwdvynq99fzjmq3aps-libsodium-1.0.20-dev/include",
    "-isystem/nix/store/88xafmpx61dwpvr1lhl6nq72wq78d1l5-lowdown-1.3.0/include",
]

FLAGS = [
    "-std=c++23",
    "-Wall",
    "-Wextra", 
    "-DNIX_VERSION=\"straylight\"",
] + INCLUDES + SYS_INCLUDES

root = Path("/home/b7r6/src/straylight/nix")
commands = []

for cpp in root.glob("src/nix/**/*.cpp"):
    commands.append({
        "directory": str(root),
        "file": str(cpp),
        "arguments": ["clang++"] + FLAGS + ["-c", str(cpp), "-o", str(cpp.with_suffix(".o"))]
    })

with open(root / "compile_commands.json", "w") as f:
    json.dump(commands, f, indent=2)

print(f"Generated {len(commands)} entries")
