R"__NIX_STR(
let
  inherit (builtins)
    attrValues
    concat_strings_sep
    isAttrs
    isBool
    mapAttrs
    ;
  inherit (import <nix/utils.nix>)
    concat_strings
    indent
    optionalString
    squash
    ;
in

# `inlineHTML` is a hack to accommodate inconsistent output from `lowdown`
{
  prefix,
  inlineHTML ? true,
}:
settingsInfo:

let

  showSetting =
    prefix: setting:
    {
      description,
      document_default,
      default_value,
      aliases,
      value,
      experimental_feature,
    }:
    let
      result = squash ''
        - ${item}

        ${indent "  " body}
      '';
      item =
        if inlineHTML then
          ''<span id="${prefix}-${setting}">[`${setting}`](#${prefix}-${setting})</span>''
        else
          "`${setting}`";
      # separate body to cleanly handle indentation
      body = ''
        ${experimentalFeatureNote}

        ${description}

        **Default:** ${showDefault document_default default_value}

        ${showAliases aliases}
      '';

      experimentalFeatureNote = optionalString (experimental_feature != null) ''
        > **Warning**
        >
        > This setting is part of an
        > [experimental feature](@docroot@/development/experimental-features.md).
        >
        > To change this setting, make sure the
        > [`${experimental_feature}` experimental feature](@docroot@/development/experimental-features.md#xp-feature-${experimental_feature})
        > is enabled.
        > For example, include the following in [`nix.conf`](@docroot@/command-ref/conf-file.md):
        >
        > ```
        > extra-experimental-features = ${experimental_feature}
        > ${setting} = ...
        > ```
      '';

      showDefault =
        document_default: default_value:
        if document_default then
          # a string_map_t value type is specified as a string, but
          # this shows the value type. The empty stringmap is `null` in
          # JSON, but that converts to `{ }` here.
          if default_value == "" || default_value == [ ] || isAttrs default_value then
            "*empty*"
          else if isBool default_value then
            if default_value then "`true`" else "`false`"
          else
            "`${toString defaultValue}`"
        else
          "*machine-specific*";

      showAliases =
        aliases:
        optionalString (aliases != [ ])
          "**Deprecated alias:** ${(concatStringsSep ", " (map (s: "`${s}`") aliases))}";

    in
    result;

in
concat_strings (attrValues (mapAttrs (showSetting prefix) settingsInfo))
)__NIX_STR"
