meta:
  id: nar
  title: Nix Archive (NAR) Format
  file-extension: nar
  endian: le
  license: MIT
  
doc: |
  NAR (Nix ARchive) is a deterministic archive format used by Nix.
  Unlike tar, NAR produces identical output for identical directory trees,
  regardless of filesystem metadata like timestamps or permissions.
  
  Structure:
    - Magic: "nix-archive-1"
    - Root node (file, directory, or symlink)
  
  All strings are padded to 8-byte boundaries.

seq:
  - id: magic
    type: padded_string
    doc: Must be "nix-archive-1"
  - id: root
    type: node
    doc: Root filesystem node

types:
  padded_string:
    doc: Length-prefixed string padded to 8-byte boundary
    seq:
      - id: len
        type: u8
      - id: data
        type: str
        size: len
        encoding: UTF-8
      - id: padding
        size: (8 - (len % 8)) % 8

  node:
    doc: A filesystem node (file, directory, or symlink)
    seq:
      - id: open_paren
        type: padded_string
        doc: Must be "("
      - id: type_tag
        type: padded_string
        doc: Must be "type"
      - id: node_type
        type: padded_string
        doc: One of "regular", "directory", "symlink"
      - id: contents
        type:
          switch-on: node_type.data
          cases:
            '"regular"': regular_file
            '"directory"': directory
            '"symlink"': symlink
      - id: close_paren
        type: padded_string
        doc: Must be ")"

  regular_file:
    doc: Regular file contents
    seq:
      - id: executable_or_contents
        type: padded_string
      - id: body
        type:
          switch-on: executable_or_contents.data
          cases:
            '"executable"': executable_file
            '"contents"': file_contents

  executable_file:
    doc: Executable file (has "executable" "" before contents)
    seq:
      - id: empty_marker
        type: padded_string
        doc: Empty string marker
      - id: contents_tag
        type: padded_string
        doc: Must be "contents"
      - id: contents
        type: file_contents

  file_contents:
    doc: Raw file data
    seq:
      - id: len
        type: u8
      - id: data
        size: len
      - id: padding
        size: (8 - (len % 8)) % 8

  directory:
    doc: Directory with sorted entries
    seq:
      - id: entries
        type: dir_entry
        repeat: until
        repeat-until: _.name.data == ")" or _io.eof

  dir_entry:
    doc: Single directory entry
    seq:
      - id: entry_tag
        type: padded_string
        doc: Must be "entry" or ")"
      - id: body
        type: dir_entry_body
        if: entry_tag.data == '"entry"'

  dir_entry_body:
    seq:
      - id: open_paren
        type: padded_string
      - id: name_tag
        type: padded_string
        doc: Must be "name"
      - id: name
        type: padded_string
        doc: Entry name (sorted lexicographically)
      - id: node_tag
        type: padded_string
        doc: Must be "node"
      - id: node
        type: node
      - id: close_paren
        type: padded_string

  symlink:
    doc: Symbolic link
    seq:
      - id: target_tag
        type: padded_string
        doc: Must be "target"
      - id: target
        type: padded_string
        doc: Symlink target path
