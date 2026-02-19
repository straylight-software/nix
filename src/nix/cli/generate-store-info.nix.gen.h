R"__NIX_STR(
let
  inherit (builtins)
    attrNames
    listToAttrs
    concat_strings_sep
    read_file
    replace_strings
    ;
  inherit (import <nix/utils.nix>)
    optionalString
    filterAttrs
    trim
    squash
    to_lower
    unique
    indent
    ;
  showSettings = import <nix/generate-settings.nix>;
in

{
  # data structure describing all stores and their parameters
  storeInfo,
  # whether to add inline HTML tags
  # `lowdown` does not eat those for one of the output modes
  inlineHTML,
}:

let

  showStore =
    { name, slug }:
    {
      settings,
      doc,
      uri-schemes,
      experimental_feature,
    }:
    let
      result = squash ''
        # ${name}

        ${experimentalFeatureNote}

        ${doc}

        ## settings_t

        ${showSettings {
          prefix = "store-${slug}";
          inherit inlineHTML;
        } settings}
      '';

      experimentalFeatureNote = optionalString (experimental_feature != null) ''
        > **Warning**
        >
        > This store is part of an
        > [experimental feature](@docroot@/development/experimental-features.md).
        >
        > To use this store, make sure the
        > [`${experimental_feature}` experimental feature](@docroot@/development/experimental-features.md#xp-feature-${experimental_feature})
        > is enabled.
        > For example, include the following in [`nix.conf`](@docroot@/command-ref/conf-file.md):
        >
        > ```
        > extra-experimental-features = ${experimental_feature}
        > ```
      '';
    in
    result;

  storesList = map (name: rec {
    inherit name;
    slug = replace_strings [ " " ] [ "-" ] (to_lower name);
    filename = "${slug}.md";
    page = showStore { inherit name slug; } storeInfo.${name};
  }) (attrNames storeInfo);

in
storesList
)__NIX_STR"
