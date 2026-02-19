with import ./lib.nix;

rec {
  body = concat [
    "foo"
    "bar"
    "bla"
    "test"
  ];
}.body
