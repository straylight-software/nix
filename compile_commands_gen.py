#!/usr/bin/env python3
"""Generate compile_commands.json for straylight/nix"""

import json
import os
from pathlib import Path

# Include paths based on the new structure
INCLUDES = [
    "-Isrc",
]

# System includes (from nix develop - extracted from g++ -v output)
SYS_INCLUDES = [
    # C++ standard library (gcc libstdc++)
    "-isystem/nix/store/mjf8jlq9grydcdvyw6hb063x5c34g5gf-gcc-15.2.0/include/c++/15.2.0",
    "-isystem/nix/store/mjf8jlq9grydcdvyw6hb063x5c34g5gf-gcc-15.2.0/include/c++/15.2.0/x86_64-unknown-linux-gnu",
    "-isystem/nix/store/mjf8jlq9grydcdvyw6hb063x5c34g5gf-gcc-15.2.0/include/c++/15.2.0/backward",
    "-isystem/nix/store/mjf8jlq9grydcdvyw6hb063x5c34g5gf-gcc-15.2.0/lib/gcc/x86_64-unknown-linux-gnu/15.2.0/include",
    "-isystem/nix/store/mjf8jlq9grydcdvyw6hb063x5c34g5gf-gcc-15.2.0/include",
    "-isystem/nix/store/mjf8jlq9grydcdvyw6hb063x5c34g5gf-gcc-15.2.0/lib/gcc/x86_64-unknown-linux-gnu/15.2.0/include-fixed",
    "-isystem/nix/store/rwalsamz4246k8f1zzxa54qx7w3fbzdg-glibc-2.42-47-dev/include",
    # Project dependencies
    "-isystem/nix/store/074p1j77fjfk52951r6x3l4kq2i62p67-boost-1.87.0-dev/include",
    "-isystem/nix/store/6zpgsxs888i5ifqkj4bbb1czf2my655n-nlohmann_json-3.12.0/include",
    "-isystem/nix/store/fgm3pz8486ksh3f94629lpb7xjr2wjp7-openssl-3.6.0-dev/include",
    "-isystem/nix/store/72hw5y7rbf4k33gnnwxkny5x4ahpi8k6-libarchive-3.8.4-dev/include",
    "-isystem/nix/store/6k6dpcdv1bxcipcgdbi89g9fxws0vhvc-sqlite-3.51.2-dev/include",
    "-isystem/nix/store/gy73p60881n9nagq7grffjgy4ycw9a85-curl-8.17.0-dev/include",
    "-isystem/nix/store/mvag6c76z2f3328c6db13798j1lg9vfy-libgit2-1.9.2-dev/include",
    "-isystem/nix/store/gm1v2azbnb96yi4fapfd1jx9f0jrnpzq-brotli-1.2.0-dev/include",
    "-isystem/nix/store/w3a55rkannhpzvfs65s0rsnphg6szahz-editline-1.17.1-unstable-2025-05-24-dev/include",
    "-isystem/nix/store/mb060ad9n7w3knx52700vr2c8q4g5w4z-libsodium-1.0.20-unstable-2025-12-31-dev/include",
    "-isystem/nix/store/p4a76cda0nh70q287v4jglzwacwwhrki-lowdown-2.0.4-dev/include",
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
