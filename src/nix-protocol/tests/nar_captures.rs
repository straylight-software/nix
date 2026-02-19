// SPDX-License-Identifier: MIT
//! Test Rust NAR serializers against captured binary data from nix-store --dump

use nix_protocol::nar::{
  FsObject, NarReader, dump_empty_directory, dump_executable, dump_string, dump_symlink,
};
use std::fs;
use std::path::PathBuf;

fn nar_captures_dir() -> PathBuf {
  PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("nar_captures")
}

fn read_capture(name: &str) -> Vec<u8> {
  let path = nar_captures_dir().join(name);
  fs::read(&path).unwrap_or_else(|e| panic!("Failed to read {}: {}", path.display(), e))
}

fn compare_nar(generated: &[u8], expected: &[u8], name: &str) {
  if generated != expected {
    eprintln!("=== {} MISMATCH ===", name);
    eprintln!(
      "Generated {} bytes, expected {} bytes",
      generated.len(),
      expected.len()
    );

    // Find first difference
    for (i, (g, e)) in generated.iter().zip(expected.iter()).enumerate() {
      if g != e {
        eprintln!(
          "First diff at offset 0x{:x}: got 0x{:02x}, expected 0x{:02x}",
          i, g, e
        );
        break;
      }
    }

    if generated.len() != expected.len() {
      eprintln!(
        "Length mismatch: generated={}, expected={}",
        generated.len(),
        expected.len()
      );
    }
  }
  assert_eq!(generated, expected, "{}: NAR mismatch", name);
}

// =============================================================================
// Tests against captured NARs
// =============================================================================

#[test]
fn test_regular_file_nar() {
  // Captured: echo "hello world" > file && nix-store --dump file
  // Content is "hello world\n" (12 bytes including newline)
  let mut buf = Vec::new();
  dump_string(&mut buf, b"hello world\n").unwrap();

  let expected = read_capture("regular_file.nar");
  compare_nar(&buf, &expected, "regular_file");
}

#[test]
fn test_executable_file_nar() {
  // Captured: echo "#!/bin/bash" > script && chmod +x script && nix-store --dump script
  // Content is "#!/bin/bash\n" (12 bytes)
  let mut buf = Vec::new();
  dump_executable(&mut buf, b"#!/bin/bash\n").unwrap();

  let expected = read_capture("executable_file.nar");
  compare_nar(&buf, &expected, "executable_file");
}

#[test]
fn test_symlink_nar() {
  // Captured: ln -s /etc/passwd link && nix-store --dump link
  let mut buf = Vec::new();
  dump_symlink(&mut buf, "/etc/passwd").unwrap();

  let expected = read_capture("symlink.nar");
  compare_nar(&buf, &expected, "symlink");
}

#[test]
fn test_empty_file_nar() {
  // Captured: touch empty && nix-store --dump empty
  let mut buf = Vec::new();
  dump_string(&mut buf, b"").unwrap();

  let expected = read_capture("empty_file.nar");
  compare_nar(&buf, &expected, "empty_file");
}

#[test]
fn test_empty_directory_nar() {
  // Captured: mkdir empty_dir && nix-store --dump empty_dir
  let mut buf = Vec::new();
  dump_empty_directory(&mut buf).unwrap();

  let expected = read_capture("empty_directory.nar");
  compare_nar(&buf, &expected, "empty_directory");
}

#[test]
fn test_directory_nar() {
  // Captured:
  //   mkdir -p dir/subdir
  //   echo "file1" > dir/a.txt
  //   echo "file2" > dir/b.txt
  //   echo "nested" > dir/subdir/c.txt
  //   nix-store --dump dir

  let tree = FsObject::directory(vec![
    ("a.txt".to_string(), FsObject::file(b"file1\n".to_vec())),
    ("b.txt".to_string(), FsObject::file(b"file2\n".to_vec())),
    (
      "subdir".to_string(),
      FsObject::directory(vec![(
        "c.txt".to_string(),
        FsObject::file(b"nested\n".to_vec()),
      )]),
    ),
  ]);

  let mut buf = Vec::new();
  tree.to_nar(&mut buf).unwrap();

  let expected = read_capture("directory.nar");
  compare_nar(&buf, &expected, "directory");
}

// =============================================================================
// Property tests
// =============================================================================

#[test]
fn test_nar_always_8_byte_aligned() {
  // Test various content sizes
  for len in 0..50 {
    let content = vec![b'x'; len];
    let mut buf = Vec::new();
    dump_string(&mut buf, &content).unwrap();
    assert_eq!(
      buf.len() % 8,
      0,
      "NAR not 8-byte aligned for content length {}",
      len
    );
  }
}

#[test]
fn test_nar_deterministic() {
  // Same content should always produce identical NAR
  for _ in 0..10 {
    let mut buf1 = Vec::new();
    let mut buf2 = Vec::new();

    dump_string(&mut buf1, b"determinism test").unwrap();
    dump_string(&mut buf2, b"determinism test").unwrap();

    assert_eq!(buf1, buf2, "NAR output should be deterministic");
  }
}

#[test]
fn test_directory_entries_sorted() {
  // Entries should be sorted regardless of input order
  let tree1 = FsObject::directory(vec![
    ("z".to_string(), FsObject::file(b"z")),
    ("a".to_string(), FsObject::file(b"a")),
    ("m".to_string(), FsObject::file(b"m")),
  ]);

  let tree2 = FsObject::directory(vec![
    ("a".to_string(), FsObject::file(b"a")),
    ("m".to_string(), FsObject::file(b"m")),
    ("z".to_string(), FsObject::file(b"z")),
  ]);

  let mut buf1 = Vec::new();
  let mut buf2 = Vec::new();

  tree1.to_nar(&mut buf1).unwrap();
  tree2.to_nar(&mut buf2).unwrap();

  assert_eq!(buf1, buf2, "Directory entries should be sorted");
}

// =============================================================================
// Round-trip tests (parse captured NAR -> compare to expected FsObject)
// =============================================================================

#[test]
fn test_parse_regular_file_nar() {
  let nar = read_capture("regular_file.nar");
  let parsed = NarReader::parse(&nar).unwrap();

  let expected = FsObject::RegularFile {
    contents: b"hello world\n".to_vec(),
    executable: false,
  };
  assert_eq!(parsed, expected);
}

#[test]
fn test_parse_executable_file_nar() {
  let nar = read_capture("executable_file.nar");
  let parsed = NarReader::parse(&nar).unwrap();

  let expected = FsObject::RegularFile {
    contents: b"#!/bin/bash\n".to_vec(),
    executable: true,
  };
  assert_eq!(parsed, expected);
}

#[test]
fn test_parse_symlink_nar() {
  let nar = read_capture("symlink.nar");
  let parsed = NarReader::parse(&nar).unwrap();

  let expected = FsObject::Symlink {
    target: "/etc/passwd".to_string(),
  };
  assert_eq!(parsed, expected);
}

#[test]
fn test_parse_empty_file_nar() {
  let nar = read_capture("empty_file.nar");
  let parsed = NarReader::parse(&nar).unwrap();

  let expected = FsObject::RegularFile {
    contents: vec![],
    executable: false,
  };
  assert_eq!(parsed, expected);
}

#[test]
fn test_parse_empty_directory_nar() {
  let nar = read_capture("empty_directory.nar");
  let parsed = NarReader::parse(&nar).unwrap();

  let expected = FsObject::Directory { entries: vec![] };
  assert_eq!(parsed, expected);
}

#[test]
fn test_parse_directory_nar() {
  let nar = read_capture("directory.nar");
  let parsed = NarReader::parse(&nar).unwrap();

  let expected = FsObject::Directory {
    entries: vec![
      (
        "a.txt".to_string(),
        FsObject::RegularFile {
          contents: b"file1\n".to_vec(),
          executable: false,
        },
      ),
      (
        "b.txt".to_string(),
        FsObject::RegularFile {
          contents: b"file2\n".to_vec(),
          executable: false,
        },
      ),
      (
        "subdir".to_string(),
        FsObject::Directory {
          entries: vec![(
            "c.txt".to_string(),
            FsObject::RegularFile {
              contents: b"nested\n".to_vec(),
              executable: false,
            },
          )],
        },
      ),
    ],
  };
  assert_eq!(parsed, expected);
}

#[test]
fn test_roundtrip_all_captures() {
  // For each capture, parse it, serialize back, and ensure byte-identical
  for name in &[
    "regular_file.nar",
    "executable_file.nar",
    "symlink.nar",
    "empty_file.nar",
    "empty_directory.nar",
    "directory.nar",
  ] {
    let original = read_capture(name);
    let parsed = NarReader::parse(&original).unwrap_or_else(|e| {
      panic!("Failed to parse {}: {:?}", name, e);
    });

    let mut reserialized = Vec::new();
    parsed
      .to_nar(&mut reserialized)
      .unwrap_or_else(|e| panic!("Failed to serialize {}: {:?}", name, e));

    assert_eq!(
      original, reserialized,
      "{}: round-trip produced different bytes",
      name
    );
  }
}
