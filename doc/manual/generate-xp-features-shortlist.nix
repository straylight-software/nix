with builtins;
with import <nix/utils.nix>;

let
  showExperimentalFeature = name: _doc: ''
    - [`${name}`](@docroot@/development/experimental-features.md#xp-feature-${name})
  '';
in
xps: indent "  " (concatStrings (attrValues (mapAttrs showExperimentalFeature xps)))
