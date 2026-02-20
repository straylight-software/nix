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
          # ── Dependencies ──────────────────────────────────────────────────────
          deps = import ./nix/deps.nix { inherit pkgs; };

          # Flatten all deps for Buck2
          allDeps =
            lib.attrValues deps.util
            ++ lib.attrValues deps.store
            ++ lib.attrValues deps.fetchers
            ++ lib.attrValues deps.expr
            ++ lib.attrValues deps.main
            ++ lib.attrValues deps.primitives
            ++ lib.attrValues deps.evring
            ++ lib.attrValues deps.language
            ++ lib.attrValues deps.test
            ++ lib.attrValues deps.bench;

          # Generate -isystem flags
          mkIncludeFlags =
            depList:
            builtins.concatStringsSep " " (
              map (pkg: if pkg ? dev then "-isystem${pkg.dev}/include" else "-isystem${pkg}/include") depList
            );

          # Generate -L and -rpath flags
          mkLibFlags =
            depList:
            builtins.concatStringsSep " " (
              map (pkg: if pkg ? lib then "-L${pkg.lib}/lib" else "-L${pkg}/lib") depList
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
              llvmpackages = pkgs.llvmPackages_19;
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

            extrapackages = allDeps;

            extrabuckconfigsections = ''

              [cxx.flags]
              c_flags = ${includeFlags}
              cxx_flags = ${includeFlags}
              link_flags = ${libFlags}
            '';

            devshellpackages = [
              pkgs.cppcheck
              pkgs.include-what-you-use
              pkgs.ast-grep
              pkgs.dhall
              pkgs.dhall-json
            ];
          };

          # ── Custom packages ───────────────────────────────────────────────────
          packages = {
            stringzilla = deps.custom.stringzilla;
            zpp_bits = deps.custom.zpp_bits;
            ngtcp2-libressl = deps.custom.ngtcp2-libressl;
          };

          # ── Formatting ────────────────────────────────────────────────────────
          treefmt = {
            projectRootFile = "flake.nix";

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
          };

          # ── Checks ────────────────────────────────────────────────────────────
          checks = {
            formatting = config.treefmt.build.check inputs.self;
          };

          # ── Default devShell ──────────────────────────────────────────────────
          devShells.default = config.devShells.sensenet-nix;
        };
    };
}
