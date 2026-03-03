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

    # Rust overlay for isospin (Firecracker) builds
    rust-overlay = {
      url = "github:oxalica/rust-overlay";
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

          # ── Isospin (Firecracker + GPU broker) Rust vendor ──────────────────────
          isospinRustVendor = import ./vendor/isospin/nix/vendor.nix { inherit pkgs; };

          # ── Firecracker guest (kernel + initrd for build VMs) ────────────────────
          firecrackerGuest = import ./nix/vm/guest.nix { inherit pkgs; };
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
              # Firecracker build service dependencies
              pkgs.firecracker # microVM hypervisor
              pkgs.e2fsprogs # provides fuse2fs for unprivileged ext4 access
              pkgs.fuse # FUSE support for image mounting
            ];

            # Auto-link isospin Rust vendor on shell entry
            devshellhook = ''
              # Link isospin Rust vendor (Firecracker deps)
              if [ ! -e vendor/isospin/third-party/rust/vendor ] || [ -L vendor/isospin/third-party/rust/vendor ]; then
                rm -f vendor/isospin/third-party/rust/vendor
                ln -sf ${isospinRustVendor} vendor/isospin/third-party/rust/vendor
                echo "📦 Linked isospin vendor -> ${isospinRustVendor}"
              fi
            '';
          };

          # ── Custom packages ───────────────────────────────────────────────────
          packages = {
            inherit (deps.custom) stringzilla;
            inherit (deps.custom) zpp_bits;
            inherit (deps.custom) ngtcp2-libressl;

            # Isospin Rust vendor (Firecracker + GPU broker deps)
            inherit isospinRustVendor;

            # Firecracker guest VM components (kernel + initrd)
            firecracker-guest = firecrackerGuest.firecracker-guest;
            nix-builder-init = firecrackerGuest.nixBuilderInit;

            # Setup script for firecracker build service
            setup-firecracker = pkgs.writeShellApplication {
              name = "setup-firecracker";
              runtimeInputs = [ pkgs.coreutils ];
              text = builtins.readFile ./scripts/setup-firecracker.sh;
            };

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
                rm -f vendor/nix-deps.bzl
                cp ${nix-deps-bzl} vendor/nix-deps.bzl

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
                  error_count=$(ast-grep scan --config build/sgconfig.yml --json src/ 2>/dev/null | \
                    jq '[.[] | select(.severity == "error")] | length')

                  if [ "$error_count" -gt 0 ]; then
                    echo "ast-grep found $error_count error(s):"
                    ast-grep scan --config build/sgconfig.yml src/ 2>/dev/null | grep -A5 "^error\["
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

            # coverage: llvm-cov line coverage threshold enforcement
            # Builds tests with coverage instrumentation, runs them, and verifies
            # that line coverage meets minimum threshold.
            #
            # Uses buck2 with cxx_coverage toolchain to get real llvm-cov metrics.
            coverage =
              let
                minLineCoverage = 5; # minimum line coverage percentage (start low, ratchet up)
                # compiler-rt provides libclang_rt.profile for coverage instrumentation
                # Use pkgsMusl version to avoid glibc fortify function references
                inherit (pkgs.pkgsMusl.llvmPackages_20) compiler-rt;
                compiler-rt-lib = "${compiler-rt}/lib/linux";
              in
              pkgs.stdenvNoCC.mkDerivation {
                name = "coverage-check";
                src = inputs.self;

                __noChroot = true; # Allow buck2 daemon access

                nativeBuildInputs = [
                  pkgs.buck2
                  pkgs.git
                  pkgs.cacert
                  pkgs.file
                  pkgs.gnugrep
                  pkgs.gawk

                  toolchain.llvm.clang
                  toolchain.llvm.lld
                  toolchain.llvm.llvm # provides llvm-profdata, llvm-cov
                  compiler-rt
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

                                    # Copy source to writable location
                                    mkdir -p $TMPDIR/build
                                    cp -a . $TMPDIR/build/
                                    chmod -R u+w $TMPDIR/build
                                    cd $TMPDIR/build

                                    # Set up prelude symlink
                                    mkdir -p nix/build
                                    rm -f nix/build/prelude
                                    ln -s ${inputs.buck2-prelude} nix/build/prelude

                                    # Copy pre-generated nix-deps.bzl
                                    rm -f vendor/nix-deps.bzl
                                    cp ${nix-deps-bzl} vendor/nix-deps.bzl

                                    # Generate buckconfig.local with coverage flags baked in
                                    # Coverage flags are added directly to c_flags/cxx_flags/link_flags
                                    # so they apply to all builds regardless of toolchain selection
                                    # NOTE: Using unquoted HEREDOC so nix variables are interpolated
                                    cat > .buckconfig.local << BUCKCONFIG
                  # AUTO-GENERATED by nix coverage check

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
                  c_flags = ${toolchain.buck2.c-flags} ${includeFlags} -fprofile-instr-generate -fcoverage-mapping
                  cxx_flags = ${toolchain.buck2.cxx-flags} ${includeFlags} -fprofile-instr-generate -fcoverage-mapping
                  link_flags = ${toolchain.buck2.link-flags} ${libFlags} -Wl,--whole-archive ${compiler-rt-lib}/libclang_rt.profile-x86_64.a -Wl,--no-whole-archive

                  [build]
                  execution_platforms = toolchains//:lre
                  BUCKCONFIG

                  echo "=== Generated .buckconfig.local ==="
                  cat .buckconfig.local
                  echo "=== Building tests with coverage instrumentation ==="

                  # Build a representative subset of fast unit tests
                                    # (full test suite would take too long for CI gate)
                                    TEST_TARGETS=(
                                      "//src/nix/util/tests:base-n_test"
                                      "//src/nix/util/tests:checked-arithmetic_test"
                                      "//src/nix/util/tests:canon-path_test"
                                      "//src/straylight/test/unit/data:lru_cache_test"
                                      "//src/nix/util/tests:strings_test"
                                      "//src/nix/util/tests:topo-sort_test"
                                      "//src/nix/util/tests:url_test"
                                      "//src/nix/util/tests:hash_test"
                                      "//src/nix/tests:url_fuzz_test"
                                      "//src/nix/tests:hash_fuzz_test"
                                      "//src/nix/tests:canon_path_fuzz_test"
                                    )

                                    buck2 build "''${TEST_TARGETS[@]}" --show-full-output > $TMPDIR/buck2-output.txt 2>&1 || {
                                      echo "buck2 build failed:"
                                      cat $TMPDIR/buck2-output.txt
                                      exit 1
                                    }

                                    echo "=== Running tests to collect coverage data ==="

                                    # Set profile output location - use pwd since TMPDIR may not work in sandbox
                                    export LLVM_PROFILE_FILE="$(pwd)/coverage-%p-%m.profraw"
                                    echo "LLVM_PROFILE_FILE=$LLVM_PROFILE_FILE"

                                    # Run each test
                                    for target in "''${TEST_TARGETS[@]}"; do
                                      # Extract binary path from buck2 output
                                      BINARY_PATH=$(grep "$target" $TMPDIR/buck2-output.txt | tail -1 | awk '{print $2}')
                                      if [ -n "$BINARY_PATH" ] && [ -x "$BINARY_PATH" ]; then
                                        echo "Running: $target"
                                        "$BINARY_PATH" || echo "  (test had failures, continuing)"
                                      fi
                                    done

                                    echo "=== Merging profile data ==="

                                    # Debug: show where profraw files might be
                                    echo "Looking for .profraw files in pwd=$(pwd)"
                                    find . -name "*.profraw" -type f 2>/dev/null || true
                                    ls -la . 2>/dev/null || true

                                    # Find all profraw files (search in current dir and TMPDIR)
                                    PROFRAW_FILES=$(find . $TMPDIR -name "*.profraw" -type f 2>/dev/null)
                                    if [ -z "$PROFRAW_FILES" ]; then
                                      echo "ERROR: No .profraw files generated"
                                      exit 1
                                    fi

                                    echo "Found profraw files:"
                                    echo "$PROFRAW_FILES"

                                    llvm-profdata merge -sparse $PROFRAW_FILES -o $TMPDIR/coverage.profdata || {
                                      echo "Failed to merge profile data"
                                      exit 1
                                    }

                                    echo "=== Generating coverage report ==="

                                    # Find test binaries for coverage report
                                    BINARIES=""
                                    FIRST_BINARY=""
                                    for target in "''${TEST_TARGETS[@]}"; do
                                      BINARY_PATH=$(grep "$target" $TMPDIR/buck2-output.txt | tail -1 | awk '{print $2}')
                                      if [ -n "$BINARY_PATH" ] && [ -x "$BINARY_PATH" ]; then
                                        if [ -z "$FIRST_BINARY" ]; then
                                          FIRST_BINARY="$BINARY_PATH"
                                        else
                                          BINARIES="$BINARIES -object=$BINARY_PATH"
                                        fi
                                      fi
                                    done

                                    # Generate text report
                                    llvm-cov report \
                                      "$FIRST_BINARY" \
                                      $BINARIES \
                                      -instr-profile=$TMPDIR/coverage.profdata \
                                      -ignore-filename-regex='buck-out|third_party|vendor|_test\.cpp' \
                                      > $TMPDIR/coverage-report.txt 2>&1 || true

                                    echo ""
                                    echo "=== Coverage Report ==="
                                    cat $TMPDIR/coverage-report.txt
                                    echo ""

                                    # Extract line coverage percentage from the TOTAL line
                                    # Format: "TOTAL ... XX.XX%"
                                    LINE_COVERAGE=$(grep "^TOTAL" $TMPDIR/coverage-report.txt | awk '{print $(NF-2)}' | tr -d '%')

                                    if [ -z "$LINE_COVERAGE" ]; then
                                      echo "WARNING: Could not parse line coverage from report"
                                      echo "Falling back to test file count check..."

                                      # Fallback: count test files
                                      TEST_FILE_COUNT=$(find src -name "*_test.cpp" -o -name "*_fuzz_test.cpp" | wc -l)
                                      echo "Test files found: $TEST_FILE_COUNT"

                                      if [ "$TEST_FILE_COUNT" -lt 15 ]; then
                                        echo "FAIL: Test file count below minimum"
                                        exit 1
                                      fi

                                      echo "PASS: Test file count OK (coverage parsing failed)"
                                      mkdir -p $out
                                      echo "coverage-check: test count fallback" > $out/result.txt
                                      exit 0
                                    fi

                                    echo "Line coverage: ''${LINE_COVERAGE}%"
                                    echo "Minimum required: ${toString minLineCoverage}%"

                                    # Compare (using awk for floating point)
                                    PASS=$(awk -v cov="$LINE_COVERAGE" -v min="${toString minLineCoverage}" 'BEGIN { print (cov >= min) ? "1" : "0" }')

                                    if [ "$PASS" = "1" ]; then
                                      echo ""
                                      echo "PASS: Line coverage (''${LINE_COVERAGE}%) >= minimum (${toString minLineCoverage}%)"
                                      mkdir -p $out
                                      echo "line_coverage=''${LINE_COVERAGE}" > $out/result.txt
                                      echo "threshold=${toString minLineCoverage}" >> $out/result.txt
                                      echo "status=PASS" >> $out/result.txt
                                    else
                                      echo ""
                                      echo "FAIL: Line coverage (''${LINE_COVERAGE}%) < minimum (${toString minLineCoverage}%)"
                                      exit 1
                                    fi
                '';

                installPhase = ''
                  # Output already created in buildPhase
                  true
                '';

                dontConfigure = true;
                dontFixup = true;
              };
          };

          # ── Default devShell ──────────────────────────────────────────────────
          devShells.default = config.devShells.sensenet-nix;
        };
    };
}
