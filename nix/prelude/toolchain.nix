# nix/prelude/toolchain.nix
#
# ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
#                              // toolchain //
# ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
#
# Compiler toolchains for MUSL static linking with UNWRAPPED clang.
#
# CRITICAL: We do NOT use clang wrappers (wrapCCWith). The nix clang wrapper
# injects -isystem paths for ALL shell dependencies via NIX_CFLAGS_COMPILE,
# which causes header/library version mismatches with vendored deps.
#
# Instead we:
#   1. Use llvm.clang-unwrapped directly (no wrapper injection)
#   2. Explicitly set ALL include paths via -isystem flags
#   3. Explicitly set ALL library paths via -L/-B flags
#   4. Use -resource-dir to point clang at the correct builtins
#
# This pattern comes from fxy's Bazel flake module.
#
# ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
{
  lib,
  pkgs,
  turing-registry,
}:
let
  inherit (pkgs.stdenv) isLinux;

  # ──────────────────────────────────────────────────────────────────────────
  #                          // llvm toolchain //
  # ──────────────────────────────────────────────────────────────────────────

  llvm = pkgs.llvmPackages_20; # TODO: move to llvm-git overlay for LLVM 22

  # UNWRAPPED clang - no NIX_CFLAGS_COMPILE injection
  inherit (llvm) clang-unwrapped;
  clang-version = lib.versions.major llvm.clang.version;

  # Resource directory for clang builtins (__stddef.h, sanitizer headers, etc.)
  # This is in the clang.cc.lib output, NOT the wrapper
  clang-resource-dir = "${llvm.clang.cc.lib}/lib/clang/${clang-version}";

  # ──────────────────────────────────────────────────────────────────────────
  #                         // musl gcc paths //
  # ──────────────────────────────────────────────────────────────────────────
  # For libstdc++ with musl. Linux only.

  musl-gcc =
    if isLinux then
      (pkgs.pkgsMusl.gcc15 or pkgs.pkgsMusl.gcc14 or pkgs.pkgsMusl.gcc13 or pkgs.pkgsMusl.gcc)
    else
      null;

  musl-gcc-unwrapped = if musl-gcc != null then musl-gcc.cc else null;
  musl-gcc-version = if musl-gcc-unwrapped != null then musl-gcc-unwrapped.version else "";
  musl-triple = if isLinux then pkgs.pkgsMusl.stdenv.hostPlatform.config else "";
  # libgcc is in lib/gcc/<triple>/<version>, but libstdc++ is in lib/
  musl-gcc-lib-gcc =
    if musl-gcc-unwrapped != null then
      "${musl-gcc-unwrapped}/lib/gcc/${musl-triple}/${musl-gcc-version}"
    else
      "";

  # libstdc++.a is in the top-level lib/ directory, NOT lib/gcc/...
  musl-gcc-lib-stdcxx = if musl-gcc-unwrapped != null then "${musl-gcc-unwrapped}/lib" else "";

  musl-gcc-paths =
    if isLinux then
      {
        include = "${musl-gcc-unwrapped}/include/c++/${musl-gcc-version}";
        include-arch = "${musl-gcc-unwrapped}/include/c++/${musl-gcc-version}/${musl-triple}";
        # lib-gcc: contains libgcc.a, crt*.o (compiler runtime)
        lib-gcc = musl-gcc-lib-gcc;
        # lib-stdcxx: contains libstdc++.a (C++ standard library)
        lib-stdcxx = musl-gcc-lib-stdcxx;
        # Legacy alias for backward compatibility
        lib = musl-gcc-lib-stdcxx;
      }
    else
      {
        include = "";
        include-arch = "";
        lib-gcc = "";
        lib-stdcxx = "";
        lib = "";
      };

  # ──────────────────────────────────────────────────────────────────────────
  #                      // NO WRAPPERS - explicit paths //
  # ──────────────────────────────────────────────────────────────────────────
  #
  # We explicitly DO NOT create wrappers. The clang wrapper (wrapCCWith) injects
  # -isystem paths for all nix shell dependencies via NIX_CFLAGS_COMPILE.
  # This causes vendored headers to be shadowed by nixpkgs headers.
  #
  # Instead, Buck2 gets the unwrapped clang binary path and we pass all include
  # and library paths explicitly via -isystem/-L/-B flags in buckconfig.

  # ──────────────────────────────────────────────────────────────────────────
  #                       // cflags / ldflags //
  # ──────────────────────────────────────────────────────────────────────────
  #
  # CRITICAL: Since we use unwrapped clang, we must explicitly set:
  #   -resource-dir    : where clang finds builtins (__stddef.h, etc.)
  #   -nostdinc        : don't search default system include paths
  #   -isystem         : add our explicit include paths (in correct order)
  #
  # Include order for C:
  #   1. clang resource-dir/include (compiler builtins)
  #   2. musl headers (libc)
  #
  # Include order for C++:
  #   1. clang resource-dir/include (compiler builtins)
  #   2. libstdc++ headers (C++ stdlib)
  #   3. libstdc++ arch-specific headers
  #   4. musl headers (libc)

  # Base C flags (musl static linking)
  musl-static-cflags = lib.concatStringsSep " " [
    # Tell clang where to find its builtins (critical for unwrapped clang)
    "-resource-dir=${clang-resource-dir}"
    # Don't search default include paths (we specify everything explicitly)
    "-nostdinc"
    # Clang builtin headers first (__stddef.h, sanitizer headers, etc.)
    "-isystem${clang-resource-dir}/include"
    # Musl libc headers
    "-isystem${pkgs.musl.dev}/include"
    # Library search paths (-B for crt*.o, libgcc; -L not used in compile phase)
    "-B${musl-gcc-paths.lib-gcc}"
    # Static linking
    "-static-libgcc"
    "-static-libstdc++"
    # Turing Registry (mandatory security flags)
    turing-registry.cflags-str
  ];

  # C++ flags add libstdc++ headers (inserted before musl in include order)
  musl-static-cxxflags = lib.concatStringsSep " " [
    # Tell clang where to find its builtins
    "-resource-dir=${clang-resource-dir}"
    # Don't search default include paths
    "-nostdinc"
    "-nostdinc++"
    # Clang builtin headers first
    "-isystem${clang-resource-dir}/include"
    # libstdc++ headers (before musl so C++ stdlib is found)
    "-isystem${musl-gcc-paths.include}"
    "-isystem${musl-gcc-paths.include-arch}"
    # Musl libc headers (after libstdc++)
    "-isystem${pkgs.musl.dev}/include"
    # Library search paths (-B for crt*.o, libgcc)
    "-B${musl-gcc-paths.lib-gcc}"
    # Static linking
    "-static-libgcc"
    "-static-libstdc++"
    # Turing Registry (mandatory security flags)
    turing-registry.cflags-str
    # C++ standard
    "-std=c++23"
  ];

  musl-static-ldflags = lib.concatStringsSep " " [
    "-static"
    # lib-stdcxx: libstdc++.a, libsupc++.a (C++ standard library)
    "-L${musl-gcc-paths.lib-stdcxx}"
    # lib-gcc: libgcc.a, crt*.o (compiler runtime)
    "-L${musl-gcc-paths.lib-gcc}"
    # musl libc
    "-L${pkgs.musl}/lib"
  ];

in
{
  inherit
    llvm
    clang-unwrapped
    clang-version
    clang-resource-dir
    musl-gcc
    musl-gcc-unwrapped
    musl-gcc-version
    musl-gcc-paths
    musl-triple
    musl-static-cflags
    musl-static-cxxflags
    musl-static-ldflags
    ;

  # ──────────────────────────────────────────────────────────────────────────
  #                        // buck2 toolchain paths //
  # ──────────────────────────────────────────────────────────────────────────
  # For .buckconfig.local generation
  #
  # CRITICAL: We use clang-unwrapped, NOT a wrapper. This avoids
  # NIX_CFLAGS_COMPILE injection that causes header version mismatches.

  buck2 = lib.optionalAttrs isLinux {
    # Tool paths - UNWRAPPED clang, no wrapper
    cc = "${clang-unwrapped}/bin/clang";
    cxx = "${clang-unwrapped}/bin/clang++";
    ar = "${llvm.bintools-unwrapped}/bin/llvm-ar";
    ld = "${llvm.bintools-unwrapped}/bin/ld.lld";

    # Include directories (explicit, no wrapper injection)
    inherit clang-resource-dir;
    musl-gcc-include = musl-gcc-paths.include;
    musl-gcc-include-arch = musl-gcc-paths.include-arch;
    musl-include = "${pkgs.musl.dev}/include";

    # Library directories
    # lib-stdcxx: libstdc++.a (C++ standard library) - in gcc's top-level lib/
    musl-gcc-lib = musl-gcc-paths.lib-stdcxx;
    # lib-gcc: libgcc.a, crt*.o (compiler runtime) - in lib/gcc/<triple>/<version>/
    musl-gcc-lib-gcc = musl-gcc-paths.lib-gcc;
    musl-lib = "${pkgs.musl}/lib";

    # Pre-built flags strings (separate C and C++ flags for correct include order)
    c-flags = musl-static-cflags;
    cxx-flags = musl-static-cxxflags;
    link-flags = musl-static-ldflags;
  };
}
