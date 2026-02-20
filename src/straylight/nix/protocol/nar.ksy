meta:
  id: nar
  title: Nix Archive (NAR) Format
  file-extension: nar
  license: MIT
  endian: le
  doc: |
    NAR (Nix Archive) is a deterministic archive format used by Nix to
    serialize filesystem objects for content-addressed storage.

    Key properties:
    - Deterministic: Same content always produces identical NAR
    - Content-addressed: Only content matters, not metadata like timestamps
    - Platform-independent: Portable across Unix-like systems

    Wire format primitives:
    - Strings: u64 length + bytes + padding to 8-byte boundary
    - File types: regular, directory, symlink

    NAR format (BNF-style):
      nar ::= "nix-archive-1" node
      
      node ::= "(" "type" node_type ")"
      
      node_type ::= regular | directory | symlink
      
      regular ::= "regular" ["executable" ""] "contents" <file_bytes>
      directory ::= "directory" entry*
      symlink ::= "symlink" "target" <target_path>
      
      entry ::= "entry" "(" "name" <filename> "node" node ")"

    Directory entries MUST be sorted lexicographically by filename (strcmp order).
    
    All strings are encoded as: u64_le(length) + bytes + zero_padding_to_8_byte_boundary

seq:
  - id: magic
    type: nar_string
    doc: Magic string, must be "nix-archive-1"
  - id: root
    type: node
    doc: Root filesystem object

types:
  # ===========================================================================
  # Wire format primitives
  # ===========================================================================

  nar_string:
    doc: |
      NAR string encoding: u64 length (LE) + bytes + padding to 8-byte boundary.
      Padding bytes are always zero.
    seq:
      - id: len
        type: u8
        doc: Length of string in bytes
      - id: data
        size: len
        type: str
        encoding: UTF-8
        doc: String content
      - id: padding
        size: (8 - (len % 8)) % 8
        doc: Zero-padding to 8-byte boundary

  nar_bytes:
    doc: |
      NAR bytes (same encoding as string but treated as binary).
      Used for file contents.
    seq:
      - id: len
        type: u8
        doc: Length in bytes
      - id: data
        size: len
        doc: Raw bytes
      - id: padding
        size: (8 - (len % 8)) % 8
        doc: Zero-padding to 8-byte boundary

  # ===========================================================================
  # Node (filesystem object)
  # ===========================================================================

  node:
    doc: |
      A filesystem object: regular file, directory, or symlink.
      Wrapped in parentheses with type field first.
    seq:
      - id: open_paren
        type: nar_string
        doc: Opening parenthesis "("
      - id: type_tag
        type: nar_string
        doc: Literal "type"
      - id: type_value
        type: nar_string
        doc: '"regular", "directory", or "symlink"'
      - id: attrs
        type:
          switch-on: type_value.data
          cases:
            '"regular"': regular_attrs
            '"directory"': directory_attrs
            '"symlink"': symlink_attrs
        doc: Type-specific attributes
      - id: close_paren
        type: nar_string
        if: type_value.data != "directory"
        doc: Closing parenthesis ")" (directories handle this in entry parsing)

  # ===========================================================================
  # Regular file
  # ===========================================================================

  regular_attrs:
    doc: |
      Attributes for a regular file:
      - Optional "executable" flag (marker is empty string)
      - "contents" followed by file bytes
    seq:
      - id: first_field
        type: nar_string
        doc: Either "executable" or "contents"
      - id: executable_marker
        type: nar_string
        if: first_field.data == "executable"
        doc: Empty string marker for executable (if executable)
      - id: contents_tag
        type: nar_string
        if: first_field.data == "executable"
        doc: Literal "contents" (if executable)
      - id: contents
        type: nar_bytes
        doc: File contents (binary data with padding)
    instances:
      is_executable:
        value: first_field.data == "executable"
        doc: Whether the file is executable
      file_data:
        value: contents.data
        doc: Raw file contents as bytes

  # ===========================================================================
  # Directory
  # ===========================================================================

  directory_attrs:
    doc: |
      Attributes for a directory: zero or more entries.
      Entries MUST be sorted lexicographically by name.
      The sequence continues until we see ")" instead of "entry".
    seq:
      - id: entries
        type: directory_entry
        repeat: until
        repeat-until: _.is_end
        doc: Directory entries, last element is the ")" marker

  directory_entry:
    doc: |
      A single directory entry.
      Format: "entry" "(" "name" <name> "node" <node> ")"
      Or just ")" to signal end of entries.
    seq:
      - id: tag
        type: nar_string
        doc: Either "entry" or ")" (end marker)
      - id: entry_content
        type: entry_body
        if: tag.data == "entry"
        doc: Entry body (only if this is an actual entry)
    instances:
      is_end:
        value: tag.data == ")"
        doc: True if this is the closing paren, not an entry

  entry_body:
    doc: |
      Body of a directory entry (after the "entry" tag).
    seq:
      - id: open_paren
        type: nar_string
        doc: Opening parenthesis "("
      - id: name_tag
        type: nar_string
        doc: Literal "name"
      - id: name
        type: nar_string
        doc: Entry filename (no slashes, no NUL, not empty, not "." or "..")
      - id: node_tag
        type: nar_string
        doc: Literal "node"
      - id: child
        type: node
        doc: The nested filesystem object
      - id: close_paren
        type: nar_string
        doc: Closing parenthesis ")"

  # ===========================================================================
  # Symlink
  # ===========================================================================

  symlink_attrs:
    doc: |
      Attributes for a symlink: just the target path.
      Note: NAR does not validate symlink targets - they can point anywhere.
    seq:
      - id: target_tag
        type: nar_string
        doc: Literal "target"
      - id: target
        type: nar_string
        doc: Symlink target path
