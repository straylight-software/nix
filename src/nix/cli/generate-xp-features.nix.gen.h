R"__NIX_STR(
with builtins;
with import <nix/utils.nix>;

let
  show_experimental_feature =
    name: doc:
    squash ''
      ## [`${name}`]{#xp-feature-${name}}

      ${doc}
    '';
in

xps: (concat_strings_sep "\n" (attrValues (mapAttrs show_experimental_feature xps)))
)__NIX_STR"
