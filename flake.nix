{
  description = "straylight/nix — nix with eval-cache disabled and modern C++23";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    systems.url = "github:nix-systems/default";
    flake-parts.url = "github:hercules-ci/flake-parts";
    treefmt-nix.url = "github:numtide/treefmt-nix";

    # sensenet - Buck2 toolchain infrastructure
    sensenet = {
      url = "git+ssh://git@github.com/straylight-software/sensenet?ref=dev";
      inputs.nixpkgs.follows = "nixpkgs";
    };
  };

  outputs =
    inputs@{ flake-parts, systems, ... }:
    flake-parts.lib.mkFlake { inherit inputs; } {
      systems = import systems;

      imports = [
        inputs.treefmt-nix.flakeModule
        inputs.sensenet.flakeModules.sensenet
      ];

      perSystem =
        { config, pkgs, ... }:
        let
          lineLength = 100;
          indentWidth = 2;

          # StringZilla C++ headers (header-only library)
          # The nixpkgs version is Python-only, so we create a simple header package
          stringzilla = pkgs.stdenv.mkDerivation {
            pname = "stringzilla";
            version = "4.5.1";
            src = pkgs.fetchFromGitHub {
              owner = "ashvardanian";
              repo = "StringZilla";
              rev = "v4.5.1";
              hash = "sha256-0T8hQ+P6gZnIX52jkRcpF1Ofxy45+B7K/feEQr5Phf0=";
            };
            dontBuild = true;
            installPhase = ''
              mkdir -p $out/include
              cp -r include/stringzilla $out/include/
            '';
          };

          # zpp_bits - High-performance C++20 binary serialization (header-only)
          zpp_bits = pkgs.stdenv.mkDerivation {
            pname = "zpp_bits";
            version = "4.6";
            src = pkgs.fetchFromGitHub {
              owner = "eyalz800";
              repo = "zpp_bits";
              rev = "v4.6";
              hash = "sha256-N3zT1eo3fLzN5/fJbfq01w7PGTD7pbmU9BMLjxsn33I=";
            };
            dontBuild = true;
            installPhase = ''
              mkdir -p $out/include
              cp zpp_bits.h $out/include/
            '';
          };

          # ngtcp2 built with LibreSSL instead of OpenSSL
          # LibreSSL 4.x has QUIC support (SSL_provide_quic_data)
          ngtcp2-libressl = pkgs.stdenv.mkDerivation {
            pname = "ngtcp2";
            version = "1.18.0";
            src = pkgs.fetchurl {
              url = "https://github.com/ngtcp2/ngtcp2/releases/download/v1.18.0/ngtcp2-1.18.0.tar.bz2";
              hash = "sha256-E7r7bFCdv2pw2WBaLIkuE/WuuTZnOZWHeKhXvHDOH6c=";
            };
            outputs = [
              "out"
              "dev"
            ];
            nativeBuildInputs = [ pkgs.cmake ];
            buildInputs = [
              pkgs.brotli
              pkgs.libev
              pkgs.nghttp3
              pkgs.libressl # Use LibreSSL instead of OpenSSL
            ];
            cmakeFlags = [
              "-DENABLE_OPENSSL=ON" # This auto-detects LibreSSL via LIBRESSL_VERSION_NUMBER
              "-DENABLE_SHARED_LIB=ON"
              "-DENABLE_STATIC_LIB=OFF"
              "-DENABLE_LIB_ONLY=ON" # Skip examples that have Linux-specific APIs
            ];
            doCheck = true;
            meta = {
              description = "ngtcp2 QUIC library built with LibreSSL";
              license = pkgs.lib.licenses.mit;
            };
          };

          # Third-party dependencies required by nix
          nixDeps = {
            # util deps
            inherit (pkgs) boost;
            inherit (pkgs) nlohmann_json;
            inherit (pkgs) libressl; # LibreSSL only - no OpenSSL
            blake3 = pkgs.libblake3;
            inherit (pkgs) brotli;
            inherit (pkgs) libsodium;
            inherit (pkgs) libarchive;
            inherit (pkgs) ada; # WHATWG URL parser
            inherit (pkgs) re2; # Fast regex engine (ERE-compatible)

            # store deps
            inherit (pkgs) sqlite;
            inherit (pkgs) curl;
            inherit (pkgs) aws-sdk-cpp;

            # fetchers deps
            inherit (pkgs) libgit2;

            # expr deps
            inherit (pkgs) boehmgc;
            inherit (pkgs) toml11;

            # main deps
            inherit (pkgs) editline;
            inherit (pkgs) lowdown;

            # test deps
            catch2 = pkgs.catch2_3;
            inherit (pkgs) rapidcheck;

            # benchmark deps
            inherit (pkgs) nanobench;

            # straylight primitives deps
            inherit stringzilla; # SIMD-accelerated string operations
            inherit zpp_bits; # High-performance binary serialization
            rapidfuzz-cpp = pkgs.rapidfuzz-cpp; # SIMD-optimized fuzzy matching
            taskflow = pkgs.taskflow; # Parallel task programming (DAG executor)

            # libevring deps
            inherit (pkgs) nghttp2; # HTTP/2 protocol library
            inherit ngtcp2-libressl; # QUIC with LibreSSL (not OpenSSL)
            inherit (pkgs) nghttp3; # HTTP/3 protocol library
            inherit (pkgs) liburing; # io_uring wrapper
            inherit (pkgs) llhttp; # HTTP/1.x parser

            # nix-language deps (WASM compilation)
            pegtl = pkgs.pegtl; # PEGTL parser combinator library
            binaryen = pkgs.binaryen; # WASM codegen
          };

          # Generate -isystem flags for all deps
          nixIncludePaths = builtins.concatStringsSep " " (
            pkgs.lib.mapAttrsToList (
              _name: pkg: if pkg ? dev then "-isystem${pkg.dev}/include" else "-isystem${pkg}/include"
            ) nixDeps
          );

          # Generate -L flags for all deps
          nixLibPaths = builtins.concatStringsSep " " (
            pkgs.lib.mapAttrsToList (
              _name: pkg: if pkg ? lib then "-L${pkg.lib}/lib" else "-L${pkg}/lib"
            ) nixDeps
          );
        in
        {
          # ─────────────────────────────────────────────────────────────────
          # sensenet project: straylight/nix
          # ─────────────────────────────────────────────────────────────────
          sensenet.projects.nix = {
            src = ./.;
            targets = [
              "//src/nix/util:util"
              "//src/nix/store:store"
              "//src/nix/expr:expr"
              "//src/nix/fetchers:fetchers"
              "//src/nix/flake:flake"
              "//src/nix/main:main"
              "//src/nix/cmd:cmd"
              "//src/nix/cli:cli"
              "//src/nix-language:language"
            ];
            toolchain = {
              cxx.enable = true;
            };
            extrapackages = pkgs.lib.attrValues nixDeps;
            extrabuckconfigsections = ''

              [cxx.flags]
              c_flags = ${nixIncludePaths}
              cxx_flags = ${nixIncludePaths}
              link_flags = ${nixLibPaths}
            '';
            devshellpackages = [
              pkgs.cppcheck
              pkgs.include-what-you-use
              pkgs.ast-grep
            ];
          };

          # ─────────────────────────────────────────────────────────────────
          # treefmt: formatters and linters
          # ─────────────────────────────────────────────────────────────────
          treefmt = {
            projectRootFile = "flake.nix";

            # `clang-format`: C/C++ formatting
            programs.clang-format.enable = true;
            programs.clang-format.includes = [
              "*.c"
              "*.h"
              "*.cpp"
              "*.hpp"
            ];

            # `deadnix`: dead code elimination for nixlang
            programs.deadnix.enable = true;

            # `nixfmt`: nixlang formatter
            programs.nixfmt.enable = true;
            programs.nixfmt.strict = true;
            programs.nixfmt.width = lineLength;

            # `statix`: static analysis for nixlang
            programs.statix.enable = true;

            # `keep-sorted`: generally tidy
            programs.keep-sorted.enable = true;

            # `shfmt`: bash formatting
            programs.shfmt.enable = true;
            programs.shfmt.indent_size = indentWidth;

            # `taplo`: TOML
            programs.taplo.enable = true;

            # `yamlfmt`: YAML
            programs.yamlfmt.enable = true;

            # `mdformat`: markdown
            programs.mdformat.enable = true;
            programs.mdformat.settings.number = true;
            programs.mdformat.settings.wrap = lineLength;
          };

          # ─────────────────────────────────────────────────────────────────
          # checks
          # ─────────────────────────────────────────────────────────────────
          checks = {
            formatting = config.treefmt.build.check inputs.self;
          };
        };
    };
}
