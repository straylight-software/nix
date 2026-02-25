# nix/deps.nix
#
# Third-party dependencies for straylight/nix.
# Centralized definition for Buck2 builds.
#
# Static linking strategy:
# - All static libs built with musl + turing registry flags
# - Header-only libs use regular pkgs
# - Custom overrides for libs that need special handling
#
{ pkgs }:
let

  # ════════════════════════════════════════════════════════════════════════════
  # Turing Registry - mandatory build flags
  # ════════════════════════════════════════════════════════════════════════════

  # ════════════════════════════════════════════════════════════════════════════
  # Musl stdenv with turing registry flags
  # ════════════════════════════════════════════════════════════════════════════

  # NOTE: Turing flags are clang-specific (-fno-limit-debug-info, -fstandalone-debug)
  # pkgsStatic/pkgsMusl use GCC, so we can't apply turing flags here.
  # Turing flags are applied in Buck2 builds via toolchain config instead.
  #
  # Helper to build with musl (no turing flags - GCC doesn't support them)
  # Just a passthrough for now, but keeps the structure for future clang builds
  with-musl-flags = drv: drv;

  # Helper to build with musl

  # ════════════════════════════════════════════════════════════════════════════
  # Custom packages not in nixpkgs
  # ════════════════════════════════════════════════════════════════════════════
  stringzilla = pkgs.callPackage ./packages/stringzilla.nix { };
  zpp_bits = pkgs.callPackage ./packages/zpp-bits.nix { };
  ngtcp2-libressl = pkgs.callPackage ./packages/ngtcp2-libressl.nix { };
  wasmtime-c-api = pkgs.callPackage ./packages/wasmtime-c-api.nix { };

  # ════════════════════════════════════════════════════════════════════════════
  # Static library builds (musl + turing registry flags)
  # ════════════════════════════════════════════════════════════════════════════

  # Static BLAKE3 without TBB (musl)
  blake3-static = with-musl-flags (
    (pkgs.libblake3.override { useTBB = false; }).overrideAttrs (old: {
      cmakeFlags = old.cmakeFlags ++ [ "-DBUILD_SHARED_LIBS=OFF" ];
    })
  );

  # Static LibreSSL (musl)
  libressl-static = with-musl-flags pkgs.pkgsStatic.libressl;

  # Static ada URL parser (musl)
  ada-static = with-musl-flags pkgs.pkgsStatic.ada;

  # Static re2 regex (musl)
  re2-static = with-musl-flags pkgs.pkgsStatic.re2;

  # Static abseil (musl) - required by re2
  abseil-static = with-musl-flags pkgs.pkgsStatic.abseil-cpp;

  # Static catch2 (musl)
  catch2-static = with-musl-flags pkgs.pkgsStatic.catch2_3;

  # Static nanobench (musl, needs -Wno-overflow for PERF_EVENT_IOC_ID)
  nanobench-static = (with-musl-flags pkgs.pkgsStatic.nanobench).overrideAttrs (old: {
    NIX_CFLAGS_COMPILE = (old.NIX_CFLAGS_COMPILE or "") + " -Wno-overflow";
  });

  # Static rapidcheck (musl)
  rapidcheck-static = with-musl-flags pkgs.pkgsStatic.rapidcheck;

  # Static boost (musl) - upstream nix uses extensively
  boost-static = with-musl-flags pkgs.pkgsStatic.boost;

  # Static brotli (musl)
  brotli-static = with-musl-flags pkgs.pkgsStatic.brotli;

  # Static libsodium (musl)
  libsodium-static = with-musl-flags pkgs.pkgsStatic.libsodium;

  # Static libarchive (musl) - override to use LibreSSL instead of OpenSSL
  # Default pkgsStatic.libarchive uses OpenSSL 3.x which has EVP_MAC_* API
  # that LibreSSL doesn't support. We override to use LibreSSL.
  libarchive-static = with-musl-flags (
    pkgs.pkgsStatic.libarchive.override { openssl = pkgs.pkgsStatic.libressl; }
  );

  # Static compression libs (transitive deps of libarchive)
  zstd-static = with-musl-flags pkgs.pkgsStatic.zstd;
  xz-static = with-musl-flags pkgs.pkgsStatic.xz; # provides liblzma
  bzip2-static = with-musl-flags pkgs.pkgsStatic.bzip2;
  zlib-static = with-musl-flags pkgs.pkgsStatic.zlib;
  acl-static = with-musl-flags pkgs.pkgsStatic.acl;
  attr-static = with-musl-flags pkgs.pkgsStatic.attr;

  # Binaryen - NOW BUILT WITH BUCK2 (vendor/binaryen/BUCK)
  # This is kept for reference but not used - Buck2 build avoids glibc __isoc23_* symbols
  binaryen-static = pkgs.binaryen;

  # Static curl with minimal dependencies, built with LibreSSL instead of OpenSSL
  # Disabled features to avoid transitive dependencies incompatible with LibreSSL:
  # - http3Support=false: HTTP/3 requires ngtcp2_crypto_ossl (OpenSSL 3.x APIs)
  # - idnSupport=false: libidn2 + libunistring adds significant complexity
  # - pslSupport=false: libpsl requires libidn2 + libunistring
  # - scpSupport=false: libssh2 adds another TLS stack dependency
  # - gsaslSupport=false: SASL auth not needed for nix fetchers
  # - ldapSupport=false: LDAP not needed for nix
  # - gssSupport=false: Kerberos/GSSAPI not needed
  # - rtmpSupport=false: RTMP streaming not needed
  # - gnutlsSupport=false: We use LibreSSL, not GnuTLS
  # - wolfsslSupport=false: We use LibreSSL, not wolfSSL
  # - rustlsSupport=false: We use LibreSSL, not rustls
  curl-static = with-musl-flags (
    pkgs.pkgsStatic.curl.override {
      openssl = pkgs.pkgsStatic.libressl; # Use LibreSSL instead of OpenSSL
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

  # libpsl not needed - pslSupport=false in curl

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
    ada = ada-static;
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
    binaryen = binaryen-static;
    wasmtime = wasmtime-c-api; # pre-built static lib from GitHub releases
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
  # NOTE: binaryen, blake3, catch2 are now built with Buck2 directly (vendor/)
  static = {
    inherit blake3-static;
    inherit libressl-static;
    inherit ada-static;
    inherit re2-static;
    inherit abseil-static;
    inherit catch2-static;
    inherit nanobench-static;
    inherit rapidcheck-static;
    inherit boost-static;
    inherit brotli-static;
    inherit libsodium-static;
    inherit libarchive-static;
    inherit wasmtime-c-api; # pre-built static lib from GitHub releases
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
      # binaryen-static removed - built with Buck2 now
      wasmtime-c-api
      ;
  };
}
