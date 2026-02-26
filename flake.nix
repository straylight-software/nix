{
  description = "straylight/nix — nix with ca-derivations, flakes, and WASM by default";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    systems.url = "github:nix-systems/default";
    flake-parts.url = "github:hercules-ci/flake-parts";
    treefmt-nix.url = "github:numtide/treefmt-nix";

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
        {
          config,
          pkgs,
          lib,
          ...
        }:
        let
          # ── Turing Registry (mandatory build flags) ────────────────────────────
          inherit (pkgs.stdenv) isLinux;
          isX86 = pkgs.stdenv.hostPlatform.isx86_64;
          turing-registry = import ./nix/prelude/turing-registry.nix { inherit lib isX86; };

          # ── Toolchain (musl static linking) ─────────────────────────────────────
          toolchain = import ./nix/prelude/toolchain.nix { inherit lib pkgs turing-registry; };

          # ── Dependencies ──────────────────────────────────────────────────────
          deps = import ./nix/deps.nix { inherit pkgs; };

          # Flatten all deps for Buck2
          # EXCLUDE vendored deps (built with Buck2, not from nixpkgs):
          #   - catch2: vendor/catch2/ (avoids glibc __libc_single_threaded symbols)
          #   - blake3: vendor/blake3/ (avoids glibc __memcpy_chk symbols)
          #   - binaryen: vendor/binaryen/ (avoids glibc __isoc23_* symbols)
          # Including these would cause header/library version mismatches.
          allDeps =
            # deps.util: exclude blake3 (vendored)
            lib.attrValues (builtins.removeAttrs deps.util [ "blake3" ])
            ++ lib.attrValues deps.store
            ++ lib.attrValues deps.fetchers
            ++ lib.attrValues deps.expr
            ++ lib.attrValues deps.main
            ++ lib.attrValues deps.primitives
            ++ lib.attrValues deps.evring
            # deps.language: exclude binaryen (vendored)
            ++ lib.attrValues (builtins.removeAttrs deps.language [ "binaryen" ])
            # deps.test: exclude catch2 (vendored)
            ++ lib.attrValues (builtins.removeAttrs deps.test [ "catch2" ])
            ++ lib.attrValues deps.bench;

          # Generate -isystem flags for third-party deps
          mkIncludeFlags =
            depList:
            builtins.concatStringsSep " " (
              map (pkg: if pkg ? dev then "-isystem${pkg.dev}/include" else "-isystem${pkg}/include") depList
            );

          # Generate -L flags for third-party deps (no rpath for static linking)
          # Order of precedence: .lib output, .out output, root package
          mkLibFlags =
            depList:
            builtins.concatStringsSep " " (
              map (
                pkg:
                if pkg ? lib then
                  "-L${pkg.lib}/lib"
                else if pkg ? out then
                  "-L${pkg.out}/lib"
                else
                  "-L${pkg}/lib"
              ) depList
            );

          includeFlags = mkIncludeFlags allDeps;
          libFlags = mkLibFlags allDeps;
        in
        {
          # ── sensenet project ──────────────────────────────────────────────────
          sensenet.projects.nix = {
            src = ./.;

            targets = [
              # Core nix libraries
              "//src/nix/util:util"
              "//src/nix/store:store"
              "//src/nix/fetchers:fetchers"
              "//src/nix/expr:expr"
              "//src/nix/flake:flake"
              "//src/nix/main:main"
              "//src/nix/cmd:cmd"
              "//src/nix/cli:cli"
              # Straylight extensions
              "//src/straylight/nix/primitives:primitives"
              "//src/straylight/evring:evring"
              "//src/straylight/language:language"
              "//src/straylight/protocol:protocol"
            ];

            toolchain.cxx = {
              enable = true;
              llvmpackages = toolchain.llvm;
            };

            remoteexecution = {
              enable = true;
              scheduler = "sense-scheduler.fly.dev";
              schedulerport = 443;
              cas = "sense-cas.fly.dev";
              casport = 443;
              tls = true;
              instancename = "main";
            };

            extrapackages =
              allDeps
              ++ (lib.optionals isLinux [
                # UNWRAPPED toolchain - no wrapper injection via NIX_CFLAGS_COMPILE
                # All include/library paths are explicit in buckconfig
                toolchain.clang-unwrapped
                toolchain.llvm.bintools-unwrapped
                toolchain.musl-gcc
                pkgs.musl
              ]);

            # Musl static linking configuration
            # Turing registry flags + musl paths + third-party deps
            extrabuckconfigsections = ''

              [cxx]
              cc = ${toolchain.buck2.cc}
              cxx = ${toolchain.buck2.cxx}
              ar = ${toolchain.buck2.ar}
              ld = ${toolchain.buck2.ld}
              clang_resource_dir = ${toolchain.buck2.clang-resource-dir}
              musl_gcc_include = ${toolchain.buck2.musl-gcc-include}
              musl_gcc_include_arch = ${toolchain.buck2.musl-gcc-include-arch}
              musl_include = ${toolchain.buck2.musl-include}
              musl_gcc_lib = ${toolchain.buck2.musl-gcc-lib}
              musl_gcc_lib_gcc = ${toolchain.buck2.musl-gcc-lib-gcc}
              musl_lib = ${toolchain.buck2.musl-lib}

              [cxx.flags]
              c_flags = ${toolchain.buck2.c-flags} ${includeFlags}
              cxx_flags = ${toolchain.buck2.cxx-flags} ${includeFlags}
              link_flags = ${toolchain.buck2.link-flags} ${libFlags}
            '';

            devshellpackages = [
              pkgs.cppcheck
              pkgs.include-what-you-use
              pkgs.ast-grep
              pkgs.dhall
              pkgs.dhall-json
              pkgs.pre-commit
            ];
          };

          # ── Custom packages ───────────────────────────────────────────────────
          packages = {
            inherit (deps.custom) stringzilla;
            inherit (deps.custom) zpp_bits;
            inherit (deps.custom) ngtcp2-libressl;
          };

          # ── Formatting ────────────────────────────────────────────────────────
          treefmt = {
            projectRootFile = "flake.nix";

            # Global excludes: vendor, tests (intentionally malformed files), generated
            settings.global.excludes = [
              "vendor/*"
              "third_party/*"
              "tests/*"
              "src/nix/expr/*-tab.cpp" # bison/flex generated
              "src/nix/expr/lexer-tab.h"
            ];

            programs.clang-format.enable = true;
            programs.clang-format.includes = [
              "*.c"
              "*.h"
              "*.cpp"
              "*.hpp"
            ];

            programs.deadnix.enable = true;
            programs.nixfmt.enable = true;
            programs.nixfmt.strict = true;
            programs.nixfmt.width = 100;

            programs.statix.enable = true;
            programs.keep-sorted.enable = true;
            programs.shfmt.enable = true;
            programs.shfmt.indent_size = 2;
            programs.taplo.enable = true;
            programs.yamlfmt.enable = true;
            programs.mdformat.enable = true;
            programs.mdformat.settings.number = true;
            programs.mdformat.settings.wrap = 100;
            # Exclude raw string literals embedded in C++ (src/nix/cli/*.md, src/nix/store/*.md)
            programs.mdformat.excludes = [
              "src/nix/cli/*.md"
              "src/nix/store/*.md"
            ];
          };

          # ── Checks ────────────────────────────────────────────────────────────
          checks = {
            formatting = config.treefmt.build.check inputs.self;

            # ast-grep: pattern-based lint rules (errors only)
            # Enforces: no-class-keyword, no-using-namespace-file-scope, no-sstream, etc.
            ast-grep =
              pkgs.runCommand "ast-grep-lint"
                {
                  nativeBuildInputs = [
                    pkgs.ast-grep
                    pkgs.jq
                  ];
                }
                ''
                  cd ${inputs.self}

                  # Run ast-grep on ALL of src/ (straylight + nix upstream)
                  error_count=$(ast-grep scan --config sgconfig.yml --json src/ 2>/dev/null | \
                    jq '[.[] | select(.severity == "error")] | length')

                  if [ "$error_count" -gt 0 ]; then
                    echo "ast-grep found $error_count error(s):"
                    ast-grep scan --config sgconfig.yml src/ 2>/dev/null | grep -A5 "^error\["
                    exit 1
                  fi

                  echo "ast-grep: no errors found"
                  touch $out
                '';

            # cppcheck: deep static analysis (inter-procedural, memory safety)
            # Catches bugs clang-tidy and ast-grep miss
            cppcheck = pkgs.runCommand "cppcheck-lint" { nativeBuildInputs = [ pkgs.cppcheck ]; } ''
              cd ${inputs.self}

              # Run cppcheck on ALL of src/ (straylight + nix upstream)
              # --error-exitcode=1 makes it fail on any error-level issue
              # Only enable 'error' severity for CI gate (not warning/performance/style)
              # Developers should run full analysis locally
              cppcheck \
                --error-exitcode=1 \
                --inline-suppr \
                --suppress=missingIncludeSystem \
                --suppress=unmatchedSuppression \
                --suppress=normalCheckLevelMaxBranches \
                --suppress=toomanyconfigs \
                --suppress=preprocessorErrorDirective \
                --std=c++23 \
                --quiet \
                src/ 2>&1 || {
                  echo "cppcheck found errors"
                  exit 1
                }

              echo "cppcheck: no errors found"
              touch $out
            '';
          };

          # ── Default devShell ──────────────────────────────────────────────────
          devShells.default = config.devShells.sensenet-nix;
        };
    };
}
