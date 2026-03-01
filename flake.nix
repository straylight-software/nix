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

    # Buck2 prelude - needed for direct buck2 builds (bypassing sensenet module)
    buck2-prelude = {
      url = "github:weyl-ai/straylight-buck2-prelude";
      flake = false;
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

          # ── Generated nix-deps.bzl (pre-built, avoids nix-build in sandbox) ────
          nix-deps-bzl = import ./nix/gen-buck-deps.nix { inherit pkgs; };

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

            # ── straylight-nix binary ─────────────────────────────────────────────
            # Direct buck2 build, bypassing sensenet flake module (which has a bug
            # trying to create symlinks in read-only source directory).
            nix = pkgs.stdenvNoCC.mkDerivation {
              name = "nix";
              src = ./.;

              __noChroot = true; # Allow buck2 daemon access

              nativeBuildInputs = [
                pkgs.buck2
                pkgs.git
                pkgs.cacert
                pkgs.file

                toolchain.llvm.clang
                toolchain.llvm.lld
                toolchain.llvm.llvm
              ]
              ++ allDeps
              ++ (lib.optionals isLinux [
                toolchain.clang-unwrapped
                toolchain.llvm.bintools-unwrapped
                toolchain.musl-gcc
                pkgs.musl
              ]);

              buildPhase = ''
                export HOME=$TMPDIR

                # We're in the unpacked source directory (read-only)
                # Copy everything to a writable location
                mkdir -p $TMPDIR/build
                cp -a . $TMPDIR/build/
                chmod -R u+w $TMPDIR/build
                cd $TMPDIR/build

                # Set up prelude symlink (remove any existing one first)
                mkdir -p nix/build
                rm -f nix/build/prelude
                ln -s ${inputs.buck2-prelude} nix/build/prelude

                # Copy pre-generated nix-deps.bzl with correct store paths for this system
                # (must be after cd to writable build dir)
                rm -f third_party/nix-deps.bzl
                cp ${nix-deps-bzl} third_party/nix-deps.bzl

                # Generate buckconfig.local with musl static linking config
                cat > .buckconfig.local << 'BUCKCONFIG'
                # AUTO-GENERATED by nix build

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

                [build]
                execution_platforms = toolchains//:lre

                [buck2_re_client]
                engine_address = grpc://sense-scheduler.fly.dev:443
                cas_address = grpc://sense-cas.fly.dev:443
                action_cache_address = grpc://sense-cas.fly.dev:443
                tls = true
                instance_name = main

                [buck2_re_client.platform_properties]
                OSFamily = linux
                container-image = nix-worker
                BUCKCONFIG

                # Build the nix CLI binary and capture output path
                buck2 build //src/nix/cli:nix --show-full-output > $TMPDIR/buck2-output.txt 2>&1 || {
                  echo "buck2 build failed:"
                  cat $TMPDIR/buck2-output.txt
                  exit 1
                }

                # Extract the output path from buck2
                # Format: "root//src/nix/cli:nix path/to/binary"
                # Use tail -1 to get the final output line (not intermediate "Waiting" lines)
                CLI_PATH=$(grep "//src/nix/cli:nix" $TMPDIR/buck2-output.txt | tail -1 | awk '{print $2}')
                echo "CLI output path: $CLI_PATH"

                if [ -n "$CLI_PATH" ] && [ -f "$CLI_PATH" ]; then
                  mkdir -p $TMPDIR/nix-bin
                  cp "$CLI_PATH" $TMPDIR/nix-bin/nix
                  chmod +x $TMPDIR/nix-bin/nix
                else
                  echo "ERROR: Could not find CLI binary from buck2 output"
                  echo "buck2 output:"
                  cat $TMPDIR/buck2-output.txt
                  exit 1
                fi
              '';

              installPhase = ''
                mkdir -p $out/bin
                install -m 755 $TMPDIR/nix-bin/nix $out/bin/nix
              '';

              dontConfigure = true;
              dontFixup = true;
            };
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
            # Catches bugs clang-tidy and ast-grep miss (ODR violations, uninitialized members, etc.)
            cppcheck = pkgs.runCommand "cppcheck-lint" { nativeBuildInputs = [ pkgs.cppcheck ]; } ''
              cd ${inputs.self}

              # Run cppcheck on ALL of src/ (straylight + nix upstream)
              # --error-exitcode=1 makes it fail on any error-level issue
              # Only enable 'error' severity for CI gate (not warning/performance/style)
              # Developers should run full analysis locally
              #
              # Suppressed false positives:
              # - unknownMacro: ANSI_* color codes, LIBCURL_VERSION, bison YY_* macros
              # - syntaxError: flex/bison generated code, C++23 syntax cppcheck doesn't parse
              cppcheck \
                --error-exitcode=1 \
                --inline-suppr \
                --suppress=missingIncludeSystem \
                --suppress=unmatchedSuppression \
                --suppress=normalCheckLevelMaxBranches \
                --suppress=toomanyconfigs \
                --suppress=preprocessorErrorDirective \
                --suppress=unknownMacro \
                --suppress=syntaxError \
                --std=c++23 \
                --quiet \
                src/ 2>&1 || {
                  echo "cppcheck found errors"
                  exit 1
                }

              echo "cppcheck: no errors found"
              touch $out
            '';

            # coverage: test count threshold enforcement
            # Verifies minimum number of test assertions exist.
            # Full coverage measurement requires buck2 devshell (run ./scripts/coverage.sh).
            #
            # This check ensures we don't regress on test count.
            coverage =
              let
                minTestFiles = 15; # minimum number of test files
                minAssertions = 300; # minimum total assertions (from TEST_COVERAGE.md baseline)
              in
              pkgs.runCommand "coverage-check"
                {
                  nativeBuildInputs = [
                    pkgs.coreutils
                    pkgs.gnugrep
                    pkgs.findutils
                  ];
                }
                ''
                  cd ${inputs.self}

                  # Count test files
                  TEST_FILE_COUNT=$(find src -name "*_test.cpp" -o -name "*_fuzz_test.cpp" | wc -l)
                  echo "Test files found: $TEST_FILE_COUNT"
                  echo "Minimum required: ${toString minTestFiles}"

                  if [ "$TEST_FILE_COUNT" -lt ${toString minTestFiles} ]; then
                    echo ""
                    echo "FAIL: Test file count ($TEST_FILE_COUNT) is below minimum (${toString minTestFiles})"
                    exit 1
                  fi

                  # Count test assertions (CHECK, REQUIRE, RC_ASSERT patterns)
                  ASSERTION_COUNT=$(grep -r -E '(CHECK|REQUIRE|RC_ASSERT|SECTION)' src --include="*_test.cpp" --include="*_fuzz_test.cpp" 2>/dev/null | wc -l)
                  echo "Test assertions found: $ASSERTION_COUNT"
                  echo "Minimum required: ${toString minAssertions}"

                  if [ "$ASSERTION_COUNT" -lt ${toString minAssertions} ]; then
                    echo ""
                    echo "FAIL: Assertion count ($ASSERTION_COUNT) is below minimum (${toString minAssertions})"
                    exit 1
                  fi

                  echo ""
                  echo "PASS: Test coverage meets minimum thresholds"
                  echo "  - Test files: $TEST_FILE_COUNT >= ${toString minTestFiles}"
                  echo "  - Assertions: $ASSERTION_COUNT >= ${toString minAssertions}"
                  touch $out
                '';
          };

          # ── Default devShell ──────────────────────────────────────────────────
          devShells.default = config.devShells.sensenet-nix;
        };
    };
}
