# nix/prelude/turing-registry.nix
#
# ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
#                          // the turing registry //
# ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
#
# Non-negotiable build flags. Code that builds under these flags can be
# debugged. Code that cannot was never real to begin with.
#
# ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
{
  lib,
  isLinux ? true,
  isX86 ? true,
}:
rec {
  # ──────────────────────────────────────────────────────────────────────────
  #                              // optimization //
  # ──────────────────────────────────────────────────────────────────────────
  # -O2 is the sweet spot: real optimizations, but debugger can still follow.

  opt-flags = [ "-O2" ];

  # ──────────────────────────────────────────────────────────────────────────
  #                                 // debug //
  # ──────────────────────────────────────────────────────────────────────────
  # Everything visible. When it breaks, you need to see inside.

  debug-flags = [
    "-g3" # maximum info (includes macros)
    "-gdwarf-5" # modern dwarf format
    "-fno-limit-debug-info" # don't truncate for speed
    "-fstandalone-debug" # full info for system headers
  ];

  # ──────────────────────────────────────────────────────────────────────────
  #                            // frame pointers //
  # ──────────────────────────────────────────────────────────────────────────
  # Stack traces work. The frame pointer is a thread through the labyrinth.

  frame-flags = [
    "-fno-omit-frame-pointer" # keep rbp/x29
    "-mno-omit-leaf-frame-pointer" # even in leaves
  ];

  # ──────────────────────────────────────────────────────────────────────────
  #                              // no theater //
  # ──────────────────────────────────────────────────────────────────────────
  # Kill hardening. These flags are security theater — they make the code
  # slower and harder to debug while providing minimal protection.

  no-harden-flags = [
    "-U_FORTIFY_SOURCE" # remove buffer "protection"
    "-D_FORTIFY_SOURCE=0" # really remove it
    "-fno-stack-protector" # no canaries
    "-fno-stack-clash-protection" # no stack clash
  ]
  ++ lib.optional isX86 "-fcf-protection=none"; # no CET on x86

  # ──────────────────────────────────────────────────────────────────────────
  #                            // the true names //
  # ──────────────────────────────────────────────────────────────────────────

  cflags = opt-flags ++ debug-flags ++ frame-flags ++ no-harden-flags;
  cxxflags = cflags ++ [ "-std=c++23" ];

  cflags-str = lib.concatStringsSep " " cflags;
  cxxflags-str = lib.concatStringsSep " " cxxflags;

  # ──────────────────────────────────────────────────────────────────────────
  #                               // the attrs //
  # ──────────────────────────────────────────────────────────────────────────
  # Derivation attributes applied to all builds.

  attrs = {
    __structuredAttrs = true;
    dontStrip = true;
    separateDebugInfo = false;
    hardeningDisable = [ "all" ];
    noAuditTmpdir = true;
  };
}
