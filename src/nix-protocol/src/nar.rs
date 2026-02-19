// SPDX-License-Identifier: MIT
//! NAR (Nix Archive) Format Serializers and Parsers
//!
//! NAR is a deterministic archive format used by Nix for content-addressed storage.
//!
//! Key properties:
//! - Deterministic: Same filesystem content always produces identical NAR
//! - Platform-independent: Portable across Unix-like systems
//! - Content-addressed: Only content matters, not metadata like timestamps
//!
//! # Wire Format
//!
//! - Strings: `u64_le(length) + bytes + zero_padding_to_8_byte_boundary`
//! - Magic: `"nix-archive-1"`
//! - Node types: `regular`, `directory`, `symlink`
//!
//! # Writing Example
//!
//! ```
//! use nix_protocol::nar::{NarWriter, dump_string};
//!
//! // Dump a simple string as a NAR
//! let mut buf = Vec::new();
//! dump_string(&mut buf, b"hello world").unwrap();
//!
//! // Or use the low-level writer
//! let mut buf = Vec::new();
//! let mut w = NarWriter::new(&mut buf);
//! w.write_magic().unwrap();
//! w.write_regular_file(b"hello", false).unwrap();
//! ```
//!
//! # Reading Example
//!
//! ```
//! use nix_protocol::nar::{NarReader, FsObject, dump_string};
//!
//! // Write a NAR
//! let mut buf = Vec::new();
//! dump_string(&mut buf, b"hello world").unwrap();
//!
//! // Parse it back
//! let obj = NarReader::parse(&buf).unwrap();
//! assert!(matches!(obj, FsObject::RegularFile { .. }));
//! ```

use std::io::{self, Read, Write};

/// NAR version magic string
pub const NAR_VERSION_MAGIC: &str = "nix-archive-1";

// =============================================================================
// Writer
// =============================================================================

/// Low-level NAR writer
pub struct NarWriter<W> {
  inner: W,
}

impl<W: Write> NarWriter<W> {
  /// Create a new NAR writer
  pub fn new(inner: W) -> Self {
    Self { inner }
  }

  /// Consume the writer and return the inner writer
  pub fn into_inner(self) -> W {
    self.inner
  }

  // ===========================================================================
  // Wire primitives
  // ===========================================================================

  /// Write a u64 in little-endian format
  pub fn write_u64(&mut self, val: u64) -> io::Result<()> {
    self.inner.write_all(&val.to_le_bytes())
  }

  /// Write padding bytes to align to 8-byte boundary
  fn write_padding(&mut self, len: usize) -> io::Result<()> {
    let pad = (8 - (len % 8)) % 8;
    if pad > 0 {
      self.inner.write_all(&[0u8; 8][..pad])?;
    }
    Ok(())
  }

  /// Write a NAR string (length-prefixed with padding)
  pub fn write_str(&mut self, s: &str) -> io::Result<()> {
    self.write_bytes(s.as_bytes())
  }

  /// Write NAR bytes (length-prefixed with padding)
  pub fn write_bytes(&mut self, data: &[u8]) -> io::Result<()> {
    self.write_u64(data.len() as u64)?;
    self.inner.write_all(data)?;
    self.write_padding(data.len())
  }

  // ===========================================================================
  // NAR structure
  // ===========================================================================

  /// Write the NAR magic header
  pub fn write_magic(&mut self) -> io::Result<()> {
    self.write_str(NAR_VERSION_MAGIC)
  }

  /// Write an opening parenthesis
  pub fn write_open(&mut self) -> io::Result<()> {
    self.write_str("(")
  }

  /// Write a closing parenthesis
  pub fn write_close(&mut self) -> io::Result<()> {
    self.write_str(")")
  }

  /// Write type field
  pub fn write_type(&mut self, type_name: &str) -> io::Result<()> {
    self.write_str("type")?;
    self.write_str(type_name)
  }

  // ===========================================================================
  // Node types
  // ===========================================================================

  /// Write a complete regular file node
  pub fn write_regular_file(&mut self, contents: &[u8], executable: bool) -> io::Result<()> {
    self.write_open()?;
    self.write_type("regular")?;
    if executable {
      self.write_str("executable")?;
      self.write_str("")?;
    }
    self.write_str("contents")?;
    self.write_bytes(contents)?;
    self.write_close()
  }

  /// Write a complete symlink node
  pub fn write_symlink(&mut self, target: &str) -> io::Result<()> {
    self.write_open()?;
    self.write_type("symlink")?;
    self.write_str("target")?;
    self.write_str(target)?;
    self.write_close()
  }

  /// Begin a directory node (call write_entry for each entry, then write_close)
  pub fn begin_directory(&mut self) -> io::Result<()> {
    self.write_open()?;
    self.write_type("directory")
  }

  /// Write a directory entry header (name only, then write the node)
  pub fn begin_entry(&mut self, name: &str) -> io::Result<()> {
    self.write_str("entry")?;
    self.write_open()?;
    self.write_str("name")?;
    self.write_str(name)?;
    self.write_str("node")
  }

  /// End a directory entry
  pub fn end_entry(&mut self) -> io::Result<()> {
    self.write_close()
  }

  /// End a directory (after all entries)
  pub fn end_directory(&mut self) -> io::Result<()> {
    self.write_close()
  }
}

// =============================================================================
// Reader
// =============================================================================

/// NAR parse error
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum NarError {
  /// Unexpected end of input
  UnexpectedEof,
  /// Invalid magic string
  InvalidMagic(String),
  /// Expected a specific token
  ExpectedToken { expected: &'static str, got: String },
  /// Invalid node type
  InvalidNodeType(String),
  /// Non-zero padding bytes
  NonZeroPadding,
  /// Invalid UTF-8 in string
  InvalidUtf8,
  /// I/O error (stringified for Clone/Eq)
  Io(String),
}

impl std::fmt::Display for NarError {
  fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
    match self {
      NarError::UnexpectedEof => write!(f, "unexpected end of NAR"),
      NarError::InvalidMagic(s) => write!(f, "invalid NAR magic: {}", s),
      NarError::ExpectedToken { expected, got } => {
        write!(f, "expected '{}', got '{}'", expected, got)
      }
      NarError::InvalidNodeType(s) => write!(f, "invalid node type: {}", s),
      NarError::NonZeroPadding => write!(f, "non-zero padding bytes"),
      NarError::InvalidUtf8 => write!(f, "invalid UTF-8 in string"),
      NarError::Io(s) => write!(f, "I/O error: {}", s),
    }
  }
}

impl std::error::Error for NarError {}

impl From<io::Error> for NarError {
  fn from(e: io::Error) -> Self {
    if e.kind() == io::ErrorKind::UnexpectedEof {
      NarError::UnexpectedEof
    } else {
      NarError::Io(e.to_string())
    }
  }
}

/// Low-level NAR reader/parser
pub struct NarReader<R> {
  inner: R,
}

impl<R: Read> NarReader<R> {
  /// Create a new NAR reader
  pub fn new(inner: R) -> Self {
    Self { inner }
  }

  /// Consume the reader and return the inner reader
  pub fn into_inner(self) -> R {
    self.inner
  }

  // ===========================================================================
  // Wire primitives
  // ===========================================================================

  /// Read a u64 in little-endian format
  pub fn read_u64(&mut self) -> Result<u64, NarError> {
    let mut buf = [0u8; 8];
    self.inner.read_exact(&mut buf)?;
    Ok(u64::from_le_bytes(buf))
  }

  /// Read and verify padding bytes (must be zeros)
  fn read_padding(&mut self, len: usize) -> Result<(), NarError> {
    let pad = (8 - (len % 8)) % 8;
    if pad > 0 {
      let mut buf = [0u8; 8];
      self.inner.read_exact(&mut buf[..pad])?;
      if buf[..pad].iter().any(|&b| b != 0) {
        return Err(NarError::NonZeroPadding);
      }
    }
    Ok(())
  }

  /// Read NAR bytes (length-prefixed with padding)
  pub fn read_bytes(&mut self) -> Result<Vec<u8>, NarError> {
    let len = self.read_u64()? as usize;
    let mut data = vec![0u8; len];
    self.inner.read_exact(&mut data)?;
    self.read_padding(len)?;
    Ok(data)
  }

  /// Read a NAR string (length-prefixed with padding)
  pub fn read_str(&mut self) -> Result<String, NarError> {
    let bytes = self.read_bytes()?;
    String::from_utf8(bytes).map_err(|_| NarError::InvalidUtf8)
  }

  /// Read and expect a specific string token
  fn expect(&mut self, expected: &'static str) -> Result<(), NarError> {
    let got = self.read_str()?;
    if got != expected {
      return Err(NarError::ExpectedToken { expected, got });
    }
    Ok(())
  }

  // ===========================================================================
  // NAR structure parsing
  // ===========================================================================

  /// Read and validate the NAR magic header
  pub fn read_magic(&mut self) -> Result<(), NarError> {
    let magic = self.read_str()?;
    if magic != NAR_VERSION_MAGIC {
      return Err(NarError::InvalidMagic(magic));
    }
    Ok(())
  }

  /// Read a complete node (file, directory, or symlink)
  pub fn read_node(&mut self) -> Result<FsObject, NarError> {
    self.expect("(")?;
    self.expect("type")?;

    let type_name = self.read_str()?;
    let node = match type_name.as_str() {
      "regular" => self.read_regular_file()?,
      "directory" => self.read_directory()?,
      "symlink" => self.read_symlink()?,
      _ => return Err(NarError::InvalidNodeType(type_name)),
    };

    Ok(node)
  }

  /// Read a regular file node (after "type" "regular")
  fn read_regular_file(&mut self) -> Result<FsObject, NarError> {
    // Next token is either "executable" or "contents"
    let first = self.read_str()?;
    let executable = first == "executable";

    if executable {
      // Read empty executable marker
      self.expect("")?;
      // Then "contents"
      self.expect("contents")?;
    } else if first != "contents" {
      return Err(NarError::ExpectedToken {
        expected: "contents",
        got: first,
      });
    }

    let contents = self.read_bytes()?;
    self.expect(")")?;

    Ok(FsObject::RegularFile {
      contents,
      executable,
    })
  }

  /// Read a directory node (after "type" "directory")
  fn read_directory(&mut self) -> Result<FsObject, NarError> {
    let mut entries = Vec::new();

    loop {
      let tag = self.read_str()?;
      if tag == ")" {
        // End of directory
        break;
      } else if tag != "entry" {
        return Err(NarError::ExpectedToken {
          expected: "entry",
          got: tag,
        });
      }

      // Read entry: "(" "name" <name> "node" <node> ")"
      self.expect("(")?;
      self.expect("name")?;
      let name = self.read_str()?;
      self.expect("node")?;
      let child = self.read_node()?;
      self.expect(")")?;

      entries.push((name, child));
    }

    Ok(FsObject::Directory { entries })
  }

  /// Read a symlink node (after "type" "symlink")
  fn read_symlink(&mut self) -> Result<FsObject, NarError> {
    self.expect("target")?;
    let target = self.read_str()?;
    self.expect(")")?;

    Ok(FsObject::Symlink { target })
  }
}

impl NarReader<&[u8]> {
  /// Parse a NAR from a byte slice
  pub fn parse(data: &[u8]) -> Result<FsObject, NarError> {
    let mut reader = NarReader::new(data);
    reader.read_magic()?;
    reader.read_node()
  }
}

impl<R: Read> NarReader<io::BufReader<R>> {
  /// Parse a NAR from a reader
  pub fn parse_from(reader: R) -> Result<FsObject, NarError> {
    let mut nar = NarReader::new(io::BufReader::new(reader));
    nar.read_magic()?;
    nar.read_node()
  }
}

// =============================================================================
// High-level API
// =============================================================================

/// Dump a string/bytes as a simple NAR (single regular file at root)
pub fn dump_string<W: Write>(w: &mut W, contents: &[u8]) -> io::Result<()> {
  let mut nar = NarWriter::new(w);
  nar.write_magic()?;
  nar.write_regular_file(contents, false)
}

/// Dump an executable file as a NAR
pub fn dump_executable<W: Write>(w: &mut W, contents: &[u8]) -> io::Result<()> {
  let mut nar = NarWriter::new(w);
  nar.write_magic()?;
  nar.write_regular_file(contents, true)
}

/// Dump a symlink as a NAR
pub fn dump_symlink<W: Write>(w: &mut W, target: &str) -> io::Result<()> {
  let mut nar = NarWriter::new(w);
  nar.write_magic()?;
  nar.write_symlink(target)
}

/// Dump an empty directory as a NAR
pub fn dump_empty_directory<W: Write>(w: &mut W) -> io::Result<()> {
  let mut nar = NarWriter::new(w);
  nar.write_magic()?;
  nar.begin_directory()?;
  nar.end_directory()
}

// =============================================================================
// Filesystem tree representation for serialization
// =============================================================================

/// A filesystem object that can be serialized to NAR
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum FsObject {
  /// Regular file with contents and executable flag
  RegularFile { contents: Vec<u8>, executable: bool },
  /// Symbolic link with target path
  Symlink { target: String },
  /// Directory with sorted entries
  Directory { entries: Vec<(String, FsObject)> },
}

impl FsObject {
  /// Create a regular file
  pub fn file(contents: impl Into<Vec<u8>>) -> Self {
    FsObject::RegularFile {
      contents: contents.into(),
      executable: false,
    }
  }

  /// Create an executable file
  pub fn executable(contents: impl Into<Vec<u8>>) -> Self {
    FsObject::RegularFile {
      contents: contents.into(),
      executable: true,
    }
  }

  /// Create a symlink
  pub fn symlink(target: impl Into<String>) -> Self {
    FsObject::Symlink {
      target: target.into(),
    }
  }

  /// Create an empty directory
  pub fn empty_dir() -> Self {
    FsObject::Directory { entries: vec![] }
  }

  /// Create a directory with entries (will be sorted)
  pub fn directory(mut entries: Vec<(String, FsObject)>) -> Self {
    entries.sort_by(|a, b| a.0.cmp(&b.0));
    FsObject::Directory { entries }
  }

  /// Serialize this filesystem object to NAR format
  pub fn to_nar<W: Write>(&self, w: &mut W) -> io::Result<()> {
    let mut nar = NarWriter::new(w);
    nar.write_magic()?;
    self.write_node(&mut nar)
  }

  fn write_node<W: Write>(&self, nar: &mut NarWriter<W>) -> io::Result<()> {
    match self {
      FsObject::RegularFile {
        contents,
        executable,
      } => {
        nar.write_regular_file(contents, *executable)?;
      }
      FsObject::Symlink { target } => {
        nar.write_symlink(target)?;
      }
      FsObject::Directory { entries } => {
        nar.begin_directory()?;
        for (name, child) in entries {
          nar.begin_entry(name)?;
          child.write_node(nar)?;
          nar.end_entry()?;
        }
        nar.end_directory()?;
      }
    }
    Ok(())
  }
}

// =============================================================================
// Tests
// =============================================================================

#[cfg(test)]
mod tests {
  use super::*;

  #[test]
  fn test_nar_magic() {
    let mut buf = Vec::new();
    let mut w = NarWriter::new(&mut buf);
    w.write_magic().unwrap();

    // "nix-archive-1" = 13 chars, padded to 16
    assert_eq!(buf.len(), 8 + 13 + 3); // len(8) + str(13) + pad(3)
    assert_eq!(&buf[8..21], b"nix-archive-1");
  }

  #[test]
  fn test_dump_string() {
    let mut buf = Vec::new();
    dump_string(&mut buf, b"hello world").unwrap();

    // Should be 8-byte aligned
    assert_eq!(buf.len() % 8, 0);

    // Check magic at start
    assert_eq!(&buf[8..21], b"nix-archive-1");
  }

  #[test]
  fn test_padding() {
    // Test various string lengths for correct padding
    for len in 0..20 {
      let content = vec![b'x'; len];
      let mut buf = Vec::new();
      dump_string(&mut buf, &content).unwrap();
      assert_eq!(buf.len() % 8, 0, "NAR not 8-byte aligned for len={}", len);
    }
  }

  #[test]
  fn test_fs_object_file() {
    let obj = FsObject::file(b"test content".to_vec());
    let mut buf = Vec::new();
    obj.to_nar(&mut buf).unwrap();

    assert_eq!(buf.len() % 8, 0);
    assert!(!buf.is_empty());
  }

  #[test]
  fn test_fs_object_directory_sorted() {
    // Entries should be sorted regardless of input order
    let obj = FsObject::directory(vec![
      ("z".to_string(), FsObject::file(b"z")),
      ("a".to_string(), FsObject::file(b"a")),
      ("m".to_string(), FsObject::file(b"m")),
    ]);

    if let FsObject::Directory { entries } = obj {
      assert_eq!(entries[0].0, "a");
      assert_eq!(entries[1].0, "m");
      assert_eq!(entries[2].0, "z");
    } else {
      panic!("Expected directory");
    }
  }

  // ===========================================================================
  // Round-trip tests (serialize -> parse -> compare)
  // ===========================================================================

  #[test]
  fn test_roundtrip_regular_file() {
    let original = FsObject::file(b"hello world\n".to_vec());
    let mut buf = Vec::new();
    original.to_nar(&mut buf).unwrap();

    let parsed = NarReader::parse(&buf).unwrap();
    assert_eq!(original, parsed);
  }

  #[test]
  fn test_roundtrip_executable_file() {
    let original = FsObject::executable(b"#!/bin/bash\necho hello\n".to_vec());
    let mut buf = Vec::new();
    original.to_nar(&mut buf).unwrap();

    let parsed = NarReader::parse(&buf).unwrap();
    assert_eq!(original, parsed);
  }

  #[test]
  fn test_roundtrip_empty_file() {
    let original = FsObject::file(b"".to_vec());
    let mut buf = Vec::new();
    original.to_nar(&mut buf).unwrap();

    let parsed = NarReader::parse(&buf).unwrap();
    assert_eq!(original, parsed);
  }

  #[test]
  fn test_roundtrip_symlink() {
    let original = FsObject::symlink("/etc/passwd");
    let mut buf = Vec::new();
    original.to_nar(&mut buf).unwrap();

    let parsed = NarReader::parse(&buf).unwrap();
    assert_eq!(original, parsed);
  }

  #[test]
  fn test_roundtrip_empty_directory() {
    let original = FsObject::empty_dir();
    let mut buf = Vec::new();
    original.to_nar(&mut buf).unwrap();

    let parsed = NarReader::parse(&buf).unwrap();
    assert_eq!(original, parsed);
  }

  #[test]
  fn test_roundtrip_directory_with_files() {
    let original = FsObject::directory(vec![
      ("a.txt".to_string(), FsObject::file(b"content a\n".to_vec())),
      ("b.txt".to_string(), FsObject::file(b"content b\n".to_vec())),
      (
        "subdir".to_string(),
        FsObject::directory(vec![(
          "c.txt".to_string(),
          FsObject::file(b"nested\n".to_vec()),
        )]),
      ),
    ]);

    let mut buf = Vec::new();
    original.to_nar(&mut buf).unwrap();

    let parsed = NarReader::parse(&buf).unwrap();
    assert_eq!(original, parsed);
  }

  #[test]
  fn test_roundtrip_complex_tree() {
    let original = FsObject::directory(vec![
      (
        "bin".to_string(),
        FsObject::directory(vec![(
          "script".to_string(),
          FsObject::executable(b"#!/bin/sh\necho hi\n".to_vec()),
        )]),
      ),
      (
        "lib".to_string(),
        FsObject::directory(vec![
          ("libfoo.so".to_string(), FsObject::symlink("libfoo.so.1")),
          (
            "libfoo.so.1".to_string(),
            FsObject::file(b"\x7fELF...".to_vec()),
          ),
        ]),
      ),
      ("README".to_string(), FsObject::file(b"Hello!\n".to_vec())),
    ]);

    let mut buf = Vec::new();
    original.to_nar(&mut buf).unwrap();

    let parsed = NarReader::parse(&buf).unwrap();
    assert_eq!(original, parsed);
  }

  #[test]
  fn test_parse_invalid_magic() {
    let bad_nar = b"\x05\x00\x00\x00\x00\x00\x00\x00wrong\x00\x00\x00";
    let result = NarReader::parse(bad_nar.as_slice());
    assert!(matches!(result, Err(NarError::InvalidMagic(_))));
  }

  #[test]
  fn test_parse_truncated() {
    let mut buf = Vec::new();
    dump_string(&mut buf, b"test").unwrap();
    buf.truncate(buf.len() / 2);

    let result = NarReader::parse(&buf);
    assert!(result.is_err());
  }
}
