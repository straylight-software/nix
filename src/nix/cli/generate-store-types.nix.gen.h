R"__NIX_STR(
let
  inherit (builtins)
    attrNames
    listToAttrs
    concat_strings_sep
    read_file
    replace_strings
    ;
  showSettings = import <nix/generate-settings.nix>;
  showStoreDocs = import <nix/generate-store-info.nix>;
in

storeInfo:

let
  storesList = showStoreDocs {
    inherit storeInfo;
    inlineHTML = true;
  };

  index =
    let
      showEntry = store: "- [${store.name}](./${store.filename})";
    in
    concat_strings_sep "\n" (map showEntry storesList);

  "index.md" = replace_strings [ "@store-types@" ] [ index ] (
    read_file ./source/store/types/index.md.in
  );

  tableOfContents =
    let
      showEntry = store: "    - [${store.name}](store/types/${store.filename})";
    in
    concat_strings_sep "\n" (map showEntry storesList) + "\n";

  "SUMMARY.md" = tableOfContents;

  storePages = listToAttrs (
    map (s: {
      name = s.filename;
      value = s.page;
    }) storesList
  );

in
storePages // { inherit "index.md" "SUMMARY.md"; }
)__NIX_STR"
