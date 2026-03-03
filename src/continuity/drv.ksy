meta:
  id: drv
  title: Nix Derivation ATerm Format
  file-extension: drv
  license: MIT

doc: |
  Nix derivations are stored in ATerm format.
  
  Structure:
    Derive(
      [(<output-name>, <output-path>, <hash-algo>, <hash>), ...],
      [(<input-drv-path>, [<output-names>]), ...],
      [<input-src-path>, ...],
      <system>,
      <builder>,
      [<arg>, ...],
      [(<env-name>, <env-value>), ...]
    )
  
  All strings are quoted with double-quotes and escaped.

seq:
  - id: derive_keyword
    contents: "Derive("
  - id: outputs
    type: output_list
  - id: comma1
    contents: ","
  - id: input_drvs
    type: input_drv_list
  - id: comma2
    contents: ","
  - id: input_srcs
    type: string_list
  - id: comma3
    contents: ","
  - id: system
    type: quoted_string
  - id: comma4
    contents: ","
  - id: builder
    type: quoted_string
  - id: comma5
    contents: ","
  - id: args
    type: string_list
  - id: comma6
    contents: ","
  - id: env
    type: env_list
  - id: close_paren
    contents: ")"

types:
  quoted_string:
    doc: Double-quoted string with escape sequences
    seq:
      - id: open_quote
        contents: '"'
      - id: value
        type: str
        terminator: 0x22  # '"'
        encoding: UTF-8
        # Note: This is simplified - real impl needs escape handling

  string_list:
    doc: List of strings: ["a","b","c"]
    seq:
      - id: open_bracket
        contents: "["
      - id: items
        type: string_list_item
        repeat: until
        repeat-until: _io.peek_byte == 0x5D  # ']'
      - id: close_bracket
        contents: "]"

  string_list_item:
    seq:
      - id: value
        type: quoted_string
      - id: comma
        size: 1
        if: _io.peek_byte == 0x2C  # ','

  output_list:
    doc: List of outputs: [("out","/nix/store/...","",""), ...]
    seq:
      - id: open_bracket
        contents: "["
      - id: items
        type: output_item
        repeat: until
        repeat-until: _io.peek_byte == 0x5D  # ']'
      - id: close_bracket
        contents: "]"

  output_item:
    doc: Single output: ("name", "path", "hashAlgo", "hash")
    seq:
      - id: open_paren
        contents: "("
      - id: name
        type: quoted_string
      - id: comma1
        contents: ","
      - id: path
        type: quoted_string
      - id: comma2
        contents: ","
      - id: hash_algo
        type: quoted_string
      - id: comma3
        contents: ","
      - id: hash
        type: quoted_string
      - id: close_paren
        contents: ")"
      - id: comma
        size: 1
        if: _io.peek_byte == 0x2C  # ','

  input_drv_list:
    doc: List of input derivations
    seq:
      - id: open_bracket
        contents: "["
      - id: items
        type: input_drv_item
        repeat: until
        repeat-until: _io.peek_byte == 0x5D  # ']'
      - id: close_bracket
        contents: "]"

  input_drv_item:
    doc: Single input drv: ("/nix/store/....drv", ["out", "dev"])
    seq:
      - id: open_paren
        contents: "("
      - id: path
        type: quoted_string
      - id: comma
        contents: ","
      - id: outputs
        type: string_list
      - id: close_paren
        contents: ")"
      - id: trailing_comma
        size: 1
        if: _io.peek_byte == 0x2C  # ','

  env_list:
    doc: List of environment variables
    seq:
      - id: open_bracket
        contents: "["
      - id: items
        type: env_item
        repeat: until
        repeat-until: _io.peek_byte == 0x5D  # ']'
      - id: close_bracket
        contents: "]"

  env_item:
    doc: Single env var: ("name", "value")
    seq:
      - id: open_paren
        contents: "("
      - id: name
        type: quoted_string
      - id: comma
        contents: ","
      - id: value
        type: quoted_string
      - id: close_paren
        contents: ")"
      - id: trailing_comma
        size: 1
        if: _io.peek_byte == 0x2C  # ','
