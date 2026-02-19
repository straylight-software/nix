{
  description = "straylight/nix — nix with eval-cache disabled and modern C++23";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    systems.url = "github:nix-systems/default";
    flake-parts.url = "github:hercules-ci/flake-parts";
    treefmt-nix.url = "github:numtide/treefmt-nix";

    # sensenet - Buck2 toolchain infrastructure (local path for dev)
    sensenet = {
      url = "path:/home/b7r6/src/straylight/sensenet";
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

          # Third-party dependencies required by nix
          nixDeps = {
            # util deps
            inherit (pkgs) boost;
            inherit (pkgs) nlohmann_json;
            inherit (pkgs) openssl;
            blake3 = pkgs.libblake3;
            inherit (pkgs) brotli;
            inherit (pkgs) libsodium;
            inherit (pkgs) libarchive;

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
