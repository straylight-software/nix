# nix/deps.nix
#
# Third-party dependencies for straylight/nix.
# Centralized definition for Buck2 builds.
#
{ pkgs }:
let
  # Custom packages not in nixpkgs
  stringzilla = pkgs.callPackage ./packages/stringzilla.nix { };
  zpp_bits = pkgs.callPackage ./packages/zpp-bits.nix { };
  ngtcp2-libressl = pkgs.callPackage ./packages/ngtcp2-libressl.nix { };
in
{
  # ── Core util deps ──────────────────────────────────────────────────────────
  util = {
    inherit (pkgs) boost;
    inherit (pkgs) nlohmann_json;
    inherit (pkgs) libressl; # LibreSSL only - no OpenSSL
    blake3 = pkgs.libblake3;
    inherit (pkgs) brotli;
    inherit (pkgs) libsodium;
    inherit (pkgs) libarchive;
    inherit (pkgs) ada; # WHATWG URL parser
    inherit (pkgs) re2; # fast regex
  };

  # ── Store deps ──────────────────────────────────────────────────────────────
  store = {
    inherit (pkgs) sqlite;
    inherit (pkgs) curl;
    inherit (pkgs) aws-sdk-cpp;
  };

  # ── Fetchers deps ───────────────────────────────────────────────────────────
  fetchers = {
    inherit (pkgs) libgit2;
  };

  # ── Expr deps ───────────────────────────────────────────────────────────────
  expr = {
    inherit (pkgs) boehmgc;
    inherit (pkgs) toml11;
  };

  # ── Main deps ───────────────────────────────────────────────────────────────
  main = {
    inherit (pkgs) editline;
    inherit (pkgs) lowdown;
  };

  # ── Straylight primitives deps ──────────────────────────────────────────────
  primitives = {
    inherit stringzilla;
    inherit zpp_bits;
    rapidfuzz-cpp = pkgs.rapidfuzz-cpp;
    taskflow = pkgs.taskflow;
  };

  # ── libevring deps (async I/O) ──────────────────────────────────────────────
  evring = {
    inherit (pkgs) nghttp2;
    inherit ngtcp2-libressl;
    inherit (pkgs) nghttp3;
    inherit (pkgs) liburing;
    inherit (pkgs) llhttp;
  };

  # ── nix-language deps (WASM) ────────────────────────────────────────────────
  language = {
    inherit (pkgs) pegtl;
    inherit (pkgs) binaryen;
  };

  # ── Test deps ───────────────────────────────────────────────────────────────
  test = {
    catch2 = pkgs.catch2_3;
    inherit (pkgs) rapidcheck;
  };

  # ── Benchmark deps ──────────────────────────────────────────────────────────
  bench = {
    inherit (pkgs) nanobench;
  };

  # ── All deps (flattened for devshell) ───────────────────────────────────────
  all =
    pkgs.lib.flatten (
      pkgs.lib.mapAttrsToList (_: v: pkgs.lib.attrValues v) {
        inherit (pkgs)
          util
          store
          fetchers
          expr
          main
          ;
      }
    )
    ++ pkgs.lib.attrValues {
      inherit stringzilla zpp_bits ngtcp2-libressl;
      rapidfuzz-cpp = pkgs.rapidfuzz-cpp;
      taskflow = pkgs.taskflow;
      inherit (pkgs)
        nghttp2
        nghttp3
        liburing
        llhttp
        ;
      inherit (pkgs) pegtl binaryen;
      catch2 = pkgs.catch2_3;
      inherit (pkgs) rapidcheck nanobench;
    };

  # ── Custom packages (for export) ────────────────────────────────────────────
  custom = {
    inherit stringzilla zpp_bits ngtcp2-libressl;
  };
}
