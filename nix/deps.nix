# nix/deps.nix
#
# Third-party dependencies for straylight/nix.
# Centralized definition for Buck2 builds.
#
# Static linking strategy:
# - Use libmodern-cpp for C++ deps (clang+musl with full DWARF-5 debug symbols)
# - Use pkgsStatic for C deps (GCC+musl, no debug symbols)
# - Header-only libs use regular pkgs
# - Custom overrides for libs that need special handling
#
{ pkgs, libmodern }:
let

  # ════════════════════════════════════════════════════════════════════════════
  # libmodern-cpp packages (clang+musl, DWARF-5, zero hardening)
  # ════════════════════════════════════════════════════════════════════════════
  # These are built with RelWithDebInfo, full debug symbols, frame pointers preserved.
  # See: vendor/libmodern-cpp/nix/prelude/turing-registry.nix

  # ════════════════════════════════════════════════════════════════════════════
  # pkgsStatic fallback for packages not yet in libmodern-cpp
  # ════════════════════════════════════════════════════════════════════════════
  # Helper - just a passthrough, kept for migration
  with-musl-flags = drv: drv;

  # ════════════════════════════════════════════════════════════════════════════
  # Custom packages not in nixpkgs or libmodern-cpp
  # ════════════════════════════════════════════════════════════════════════════
  stringzilla = pkgs.callPackage ./packages/stringzilla.nix { };
  zpp_bits = pkgs.callPackage ./packages/zpp-bits.nix { };

  # ════════════════════════════════════════════════════════════════════════════
  # libmodern-cpp packages (clang+musl, DWARF-5, zero hardening)
  # ════════════════════════════════════════════════════════════════════════════
  # These replace the old pkgsStatic versions with properly debuggable builds.

  # Core
  abseil-static = libmodern.abseil-cpp;
  fmt-static = libmodern.fmt;
  libsodium-static = libmodern.libsodium;
  re2-static = libmodern.re2;

  # Crypto
  blake3-static = libmodern.blake3;
  libressl-static = libmodern.libressl;

  # Data
  sqlite-static = libmodern.sqlite;
  libgit2-static = libmodern.libgit2;
  ada-static = libmodern.ada;

  # Allocator
  mimalloc-static = libmodern.mimalloc;

  # Test/Bench
  catch2-static = libmodern.catch2;
  nanobench-static = libmodern.nanobench;
  rapidcheck-static = libmodern.rapidcheck;

  # WASM
  wasmtime-c-api = libmodern.wasmtime;

  # Async I/O
  liburing-static = libmodern.liburing;
  llhttp-static = libmodern.llhttp;
  nghttp2-static = libmodern.nghttp2;
  nghttp3-static = libmodern.nghttp3;
  ngtcp2-static = libmodern.ngtcp2;

  # ════════════════════════════════════════════════════════════════════════════
  # pkgsStatic fallbacks (packages not yet in libmodern-cpp)
  # ════════════════════════════════════════════════════════════════════════════

  # Static boost (musl) - upstream nix uses extensively
  # TODO: Add to libmodern-cpp
  boost-static = with-musl-flags pkgs.pkgsStatic.boost;

  # Static brotli (musl)
  # TODO: Add to libmodern-cpp
  brotli-static = with-musl-flags pkgs.pkgsStatic.brotli;

  # Static libarchive (musl) - override to use LibreSSL instead of OpenSSL
  # TODO: Add to libmodern-cpp
  libarchive-static = with-musl-flags (
    pkgs.pkgsStatic.libarchive.override { openssl = libressl-static; }
  );

  # Static compression libs (transitive deps of libarchive)
  # TODO: Add to libmodern-cpp
  zstd-static = with-musl-flags pkgs.pkgsStatic.zstd;
  xz-static = with-musl-flags pkgs.pkgsStatic.xz; # provides liblzma
  bzip2-static = with-musl-flags pkgs.pkgsStatic.bzip2;
  zlib-static = with-musl-flags pkgs.pkgsStatic.zlib;
  acl-static = with-musl-flags pkgs.pkgsStatic.acl;
  attr-static = with-musl-flags pkgs.pkgsStatic.attr;

  # Static curl with minimal dependencies, built with LibreSSL
  # TODO: Add to libmodern-cpp with HTTP/3 support using ngtcp2
  curl-static = with-musl-flags (
    pkgs.pkgsStatic.curl.override {
      openssl = libressl-static;
      http3Support = false;
      idnSupport = false;
      pslSupport = false;
      scpSupport = false;
      gsaslSupport = false;
      ldapSupport = false;
      gssSupport = false;
      rtmpSupport = false;
      gnutlsSupport = false;
      wolfsslSupport = false;
      rustlsSupport = false;
    }
  );

in
{
  # ── Core util deps ──────────────────────────────────────────────────────────
  util = {
    boost = boost-static;
    inherit (pkgs) nlohmann_json; # header-only
    libressl = libressl-static;
    blake3 = blake3-static;
    brotli = brotli-static;
    libsodium = libsodium-static;
    libarchive = libarchive-static;
    # Compression libs (transitive deps of libarchive)
    zstd = zstd-static;
    xz = xz-static; # provides liblzma
    bzip2 = bzip2-static;
    zlib = zlib-static;
    # ACL/attr libs (transitive deps of libarchive)
    acl = acl-static;
    attr = attr-static;
    # ada: vendored in third_party/ada (built with Buck2)
    re2 = re2-static;
    abseil = abseil-static;
  };

  # ── Store deps ──────────────────────────────────────────────────────────────
  # sqlite and libgit2 are vendored (built with Buck2)
  # curl uses pkgsStatic for static musl linking with minimal deps
  store = {
    # sqlite: vendored in vendor/sqlite/
    # libgit2: vendored in vendor/libgit2/
    curl = curl-static;
    # aws-sdk-cpp: stub (not needed for core nix)
  };

  # ── Fetchers deps ───────────────────────────────────────────────────────────
  fetchers = {
    # libgit2: vendored in vendor/libgit2/
  };

  # ── Expr deps ───────────────────────────────────────────────────────────────
  expr = {
    boehmgc = with-musl-flags pkgs.pkgsStatic.boehmgc;
    inherit (pkgs) toml11; # header-only
  };

  # ── Performance deps ────────────────────────────────────────────────────────
  perf = {
    # mimalloc - fast allocator to replace musl's malloc (reduces lock contention)
    mimalloc = mimalloc-static;
  };

  # ── Main deps ───────────────────────────────────────────────────────────────
  main = {
    editline = with-musl-flags pkgs.pkgsStatic.editline;
    lowdown = with-musl-flags pkgs.pkgsStatic.lowdown;
    ncurses = with-musl-flags pkgs.pkgsStatic.ncurses; # Required by editline
  };

  # ── Straylight primitives deps ──────────────────────────────────────────────
  primitives = {
    inherit stringzilla; # header-only
    inherit zpp_bits; # header-only
    inherit (pkgs) rapidfuzz-cpp; # header-only
    inherit (pkgs) taskflow; # header-only
  };

  # ── libevring deps (async I/O) ──────────────────────────────────────────────
  # Now using libmodern-cpp static builds with full debug symbols
  evring = {
    nghttp2 = nghttp2-static;
    ngtcp2 = ngtcp2-static;
    nghttp3 = nghttp3-static;
    liburing = liburing-static;
    llhttp = llhttp-static;
  };

  # ── nix-language deps (WASM) ────────────────────────────────────────────────
  language = {
    inherit (pkgs) pegtl; # header-only
    # binaryen: built with Buck2 (vendor/binaryen/) - libmodern-cpp build disabled
    wasmtime = wasmtime-c-api;
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
  # Most C++ deps now come from libmodern-cpp (clang+musl, full debug symbols)
  static = {
    # libmodern-cpp packages
    inherit blake3-static;
    inherit libressl-static;
    inherit re2-static;
    inherit abseil-static;
    inherit catch2-static;
    inherit nanobench-static;
    inherit rapidcheck-static;
    inherit libsodium-static;
    inherit mimalloc-static;
    inherit sqlite-static;
    inherit libgit2-static;
    inherit ada-static;
    inherit wasmtime-c-api;
    inherit liburing-static;
    inherit llhttp-static;
    inherit nghttp2-static;
    inherit nghttp3-static;
    inherit ngtcp2-static;
    inherit fmt-static;
    # pkgsStatic fallbacks
    inherit boost-static;
    inherit brotli-static;
    inherit libarchive-static;
  };

  # ── Custom packages (for export) ────────────────────────────────────────────
  custom = {
    inherit
      stringzilla
      zpp_bits
      blake3-static
      libressl-static
      re2-static
      catch2-static
      nanobench-static
      rapidcheck-static
      wasmtime-c-api
      ;
    # Alias for backward compatibility - libmodern ngtcp2 is built with libressl
    ngtcp2-libressl = ngtcp2-static;
  };
}
