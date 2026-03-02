meta:
  id: narinfo
  title: Nix Binary Cache NAR Info
  file-extension: narinfo
  license: MIT

doc: |
  .narinfo files describe store paths in binary caches.
  They contain metadata about a NAR archive and how to fetch it.
  
  Format is line-based key-value pairs:
    StorePath: /nix/store/<hash>-<name>
    URL: nar/<hash>.nar.xz
    Compression: xz
    FileHash: sha256:<base64>
    FileSize: <bytes>
    NarHash: sha256:<base64>
    NarSize: <bytes>
    References: <space-separated store paths>
    Deriver: <drv path>
    Sig: <signature>

seq:
  - id: lines
    type: line
    repeat: until
    repeat-until: _io.eof

types:
  line:
    seq:
      - id: key
        type: str
        terminator: 0x3A  # ':'
        encoding: UTF-8
      - id: space
        size: 1
      - id: value
        type: str
        terminator: 0x0A  # '\n'
        encoding: UTF-8

instances:
  store_path:
    value: 'lines.size > 0 and lines[0].key == "StorePath" ? lines[0].value : ""'
  url:
    value: 'lines.size > 1 and lines[1].key == "URL" ? lines[1].value : ""'
  compression:
    value: 'lines.size > 2 and lines[2].key == "Compression" ? lines[2].value : ""'
  file_hash:
    value: 'lines.size > 3 and lines[3].key == "FileHash" ? lines[3].value : ""'
  file_size:
    value: 'lines.size > 4 and lines[4].key == "FileSize" ? lines[4].value : ""'
  nar_hash:
    value: 'lines.size > 5 and lines[5].key == "NarHash" ? lines[5].value : ""'
  nar_size:
    value: 'lines.size > 6 and lines[6].key == "NarSize" ? lines[6].value : ""'
