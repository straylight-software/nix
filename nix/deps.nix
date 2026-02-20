# nix/deps.nix
#
# Third-party dependencies for straylight/nix.
# Centralized definition for Buck2 builds.
#
# Static linking strategy:
# - Use pkgsStatic (musl) for runtime libs to avoid glibc
# - Header-only libs use regular pkgs
# - Custom overrides for libs that need special handling
#
{ pkgs }:
let
  # Custom packages not in nixpkgs
  stringzilla = pkgs.callPackage ./packages/stringzilla.nix { };
  zpp_bits = pkgs.callPackage ./packages/zpp-bits.nix { };
  ngtcp2-libressl = pkgs.callPackage ./packages/ngtcp2-libressl.nix { };

  # ════════════════════════════════════════════════════════════════════════════
  # Static library builds (musl, no glibc)
  # ════════════════════════════════════════════════════════════════════════════

  # Static BLAKE3 without TBB
  # TODO[b7r6]: confirm TBB performance improvement - TBB enables parallel
  #             hashing for large files which is usually a win, but requires
  #             also linking TBB (dynamically or statically).
  blake3-static = (pkgs.libblake3.override { useTBB = false; }).overrideAttrs (old: {
    cmakeFlags = old.cmakeFlags ++ [ "-DBUILD_SHARED_LIBS=OFF" ];
  });

  # Static LibreSSL (musl)
  libressl-static = pkgs.pkgsStatic.libressl;

  # Static ada URL parser
  ada-static = pkgs.pkgsStatic.ada;

  # Static re2 regex
  re2-static = pkgs.pkgsStatic.re2;

  # Static catch2
  catch2-static = pkgs.pkgsStatic.catch2_3;

  # Static nanobench (needs -Wno-overflow for musl PERF_EVENT_IOC_ID issue)
  nanobench-static = pkgs.pkgsStatic.nanobench.overrideAttrs (old: {
    NIX_CFLAGS_COMPILE = (old.NIX_CFLAGS_COMPILE or "") + " -Wno-overflow";
  });

  # Static rapidcheck (musl)
  rapidcheck-static = pkgs.pkgsStatic.rapidcheck;
in
{
  # ── Core util deps ──────────────────────────────────────────────────────────
  # Note: boost removed - using std::vector instead of boost::container::small_vector
  util = {
    inherit (pkgs) nlohmann_json; # header-only
    libressl = libressl-static;
    blake3 = blake3-static;
    inherit (pkgs) brotli; # TODO: convert to static
    inherit (pkgs) libsodium; # TODO: convert to static
    inherit (pkgs) libarchive; # TODO: convert to static
    ada = ada-static;
    re2 = re2-static;
  };

  # ── Store deps ──────────────────────────────────────────────────────────────
  store = {
    inherit (pkgs) sqlite; # TODO: convert to static
    inherit (pkgs) curl; # TODO: convert to static
    inherit (pkgs) aws-sdk-cpp; # TODO: convert to static
  };

  # ── Fetchers deps ───────────────────────────────────────────────────────────
  fetchers = {
    inherit (pkgs) libgit2; # TODO: convert to static
  };

  # ── Expr deps ───────────────────────────────────────────────────────────────
  expr = {
    inherit (pkgs) boehmgc; # TODO: convert to static
    inherit (pkgs) toml11; # header-only
  };

  # ── Main deps ───────────────────────────────────────────────────────────────
  main = {
    inherit (pkgs) editline; # TODO: convert to static
    inherit (pkgs) lowdown; # TODO: convert to static
  };

  # ── Straylight primitives deps ──────────────────────────────────────────────
  primitives = {
    inherit stringzilla; # header-only
    inherit zpp_bits; # header-only
    rapidfuzz-cpp = pkgs.rapidfuzz-cpp; # header-only
    taskflow = pkgs.taskflow; # header-only
  };

  # ── libevring deps (async I/O) ──────────────────────────────────────────────
  evring = {
    inherit (pkgs) nghttp2; # TODO: convert to static
    inherit ngtcp2-libressl;
    inherit (pkgs) nghttp3; # TODO: convert to static
    inherit (pkgs) liburing; # TODO: convert to static
    inherit (pkgs) llhttp; # TODO: convert to static
  };

  # ── nix-language deps (WASM) ────────────────────────────────────────────────
  language = {
    inherit (pkgs) pegtl; # header-only
    inherit (pkgs) binaryen; # TODO: convert to static (build takes long)
  };

  # ── Test deps ───────────────────────────────────────────────────────────────
  test = {
    catch2 = catch2-static;
    rapidcheck = rapidcheck-static;
  };

  # ── Benchmark deps ──────────────────────────────────────────────────────────
  bench = {
    nanobench = nanobench-static;
  };

  # ── Static libs for Buck2 prebuilt_cxx_library ──────────────────────────────
  # These are used by gen-buck-deps.nix to generate nix-deps.bzl
  static = {
    inherit blake3-static;
    inherit libressl-static;
    inherit ada-static;
    inherit re2-static;
    inherit catch2-static;
    inherit nanobench-static;
    inherit rapidcheck-static;
  };

  # ── Custom packages (for export) ────────────────────────────────────────────
  custom = {
    inherit
      stringzilla
      zpp_bits
      ngtcp2-libressl
      blake3-static
      libressl-static
      ada-static
      re2-static
      catch2-static
      nanobench-static
      rapidcheck-static
      ;
  };
}
