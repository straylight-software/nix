{
  description = "straylight/nix — nix with eval-cache disabled and modern C++23";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    systems.url = "github:nix-systems/default";
    flake-parts.url = "github:hercules-ci/flake-parts";
    treefmt-nix.url = "github:numtide/treefmt-nix";

    # LLVM 22 from git - matches sensenet
    llvm-project = {
      url = "github:llvm/llvm-project/bb1f220d534b0f6d80bea36662f5188ff11c2e54";
      flake = false;
    };
  };

  outputs =
    inputs@{ flake-parts, systems, ... }:
    flake-parts.lib.mkFlake { inherit inputs; } {
      systems = import systems;

      imports = [ inputs.treefmt-nix.flakeModule ];

      perSystem =
        {
          config,
          pkgs,
          system,
          ...
        }:
        let
          lineLength = 100;
          indentWidth = 2;

          # LLVM 22 from git with SM120 Blackwell support
          llvm-git = pkgs.stdenv.mkDerivation {
            pname = "llvm-git";
            version = "22.0.0-git";

            src = inputs.llvm-project;

            sourceRoot = "source/llvm";

            nativeBuildInputs = [
              pkgs.cmake
              pkgs.ninja
              pkgs.python3
            ];

            buildInputs = [
              pkgs.libxml2
              pkgs.zlib
              pkgs.ncurses
              pkgs.libffi
            ];

            cmakeFlags = [
              "-DLLVM_ENABLE_PROJECTS=clang;clang-tools-extra;lld"
              "-DCMAKE_BUILD_TYPE=Release"
              "-DLLVM_TARGETS_TO_BUILD=X86;NVPTX;AArch64"
              "-DLLVM_ENABLE_ASSERTIONS=OFF"
              "-DLLVM_INSTALL_UTILS=ON"
              "-DLLVM_BUILD_TOOLS=ON"
              "-DLLVM_INCLUDE_TESTS=OFF"
              "-DLLVM_INCLUDE_EXAMPLES=OFF"
              "-DLLVM_INCLUDE_DOCS=OFF"
            ];

            enableParallelBuilding = true;

            meta = {
              description = "LLVM/Clang from git with CUDA 13 and SM120 Blackwell support";
              homepage = "https://llvm.org";
              license = pkgs.lib.licenses.ncsa;
              platforms = pkgs.lib.platforms.linux;
            };
          };
        in
        {
          # ─────────────────────────────────────────────────────────────────
          # treefmt: formatters and linters
          # ─────────────────────────────────────────────────────────────────
          treefmt = {
            projectRootFile = "flake.nix";

            # `clang-format`: C/C++ formatting (uses llvm-git)
            programs.clang-format.enable = true;
            programs.clang-format.package = llvm-git;
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
          # packages
          # ─────────────────────────────────────────────────────────────────
          packages = {
            inherit llvm-git;
            default = llvm-git;
          };

          # ─────────────────────────────────────────────────────────────────
          # devShells
          # ─────────────────────────────────────────────────────────────────
          devShells.default = pkgs.mkShell {
            name = "straylight-nix-dev";

            packages = [
              # C++ toolchain (LLVM 22 from git)
              llvm-git

              # Build tools
              pkgs.buck2
              pkgs.ninja
              pkgs.pkg-config
              pkgs.cmake
              pkgs.meson

              # Static analysis
              pkgs.cppcheck
              pkgs.include-what-you-use

              # Dependencies (from original nix)
              pkgs.boost
              pkgs.nlohmann_json
              pkgs.openssl
              pkgs.libarchive
              pkgs.sqlite
              pkgs.curl
              pkgs.libgit2
              pkgs.brotli
              pkgs.editline
              pkgs.libsodium
              pkgs.lowdown
              pkgs.busybox-sandbox-shell
              pkgs.libcpuid
              pkgs.toml11
              pkgs.pegtl

              # Formatters (from treefmt)
              config.treefmt.build.wrapper
            ];

            shellHook = ''
              echo ""
              echo "  straylight/nix dev shell"
              echo "  ────────────────────────"
              echo ""
              echo "  nix fmt                    # run treefmt"
              echo "  clang-tidy src/nix/util/*.cpp -- -std=c++23 -I src"
              echo "  cppcheck --enable=all src/nix/"
              echo ""
            '';
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
