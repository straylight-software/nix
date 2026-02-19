R"__NIX_STR(
attrs@{
  drv_path,
  outputs,
  name,
  ...
}:

let

  commonAttrs = (builtins.listToAttrs outputsList) // {
    all = map (x: x.value) outputsList;
    inherit drv_path name;
    type = "derivation";
  };

  outputToAttrListElement = output_name: {
    name = output_name;
    value = commonAttrs // {
      out_path = builtins.get_attr output_name attrs;
      inherit output_name;
    };
  };

  outputsList = map outputToAttrListElement outputs;

in
(builtins.head outputsList).value
)__NIX_STR"
