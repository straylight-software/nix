R"__NIX_STR(
with builtins;
with import <nix/utils.nix>;

let
  show_experimental_feature = name: doc: ''
    - [`${name}`](@docroot@/development/experimental-features.md#xp-feature-${name})
  '';
in
xps: indent "  " (concat_strings (attrValues (mapAttrs show_experimental_feature xps)))
)__NIX_STR"
