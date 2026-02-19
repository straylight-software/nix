// SPDX-License-Identifier: MIT
//! Test Rust serializers and parsers against captured binary data

use nix_protocol::*;
use std::fs;
use std::io::Cursor;
use std::path::PathBuf;

fn captures_dir() -> PathBuf {
  PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("captures")
}

fn read_capture(name: &str) -> Vec<u8> {
  let path = captures_dir().join(name);
  fs::read(&path).unwrap_or_else(|e| panic!("Failed to read {}: {}", path.display(), e))
}

fn compare_buffers(generated: &[u8], expected: &[u8], name: &str) {
  assert_eq!(
    generated.len(),
    expected.len(),
    "{}: size mismatch (got {}, expected {})",
    name,
    generated.len(),
    expected.len()
  );

  for (i, (g, e)) in generated.iter().zip(expected.iter()).enumerate() {
    assert_eq!(
      g, e,
      "{}: byte mismatch at offset {} (got 0x{:02x}, expected 0x{:02x})",
      name, i, g, e
    );
  }
}

// Helper to parse a string from captured binary data
fn parse_string(data: &[u8], offset: usize) -> (String, usize) {
  let len = u64::from_le_bytes(data[offset..offset + 8].try_into().unwrap()) as usize;
  let s = String::from_utf8(data[offset + 8..offset + 8 + len].to_vec()).unwrap();
  let padding = (8 - (len % 8)) % 8;
  (s, offset + 8 + len + padding)
}

// =============================================================================
// Tests
// =============================================================================

#[test]
fn test_client_hello() {
  let mut buf = Vec::new();
  let mut w = Writer::new(&mut buf);
  write_client_hello(&mut w, 0x0126).unwrap();

  let expected = read_capture("client_hello.bin");
  compare_buffers(&buf, &expected, "client_hello");
}

#[test]
fn test_server_hello() {
  let mut buf = Vec::new();
  let mut w = Writer::new(&mut buf);
  write_server_hello(&mut w, 0x0126).unwrap();

  let expected = read_capture("server_hello.bin");
  compare_buffers(&buf, &expected, "server_hello");
}

#[test]
fn test_isvalidpath_request() {
  let captured = read_capture("isvalidpath_request.bin");
  let (path, _) = parse_string(&captured, 8);

  let mut buf = Vec::new();
  let mut w = Writer::new(&mut buf);
  write_is_valid_path_request(&mut w, &path).unwrap();

  compare_buffers(&buf, &captured, "isvalidpath_request");
}

#[test]
fn test_querypathinfo_request() {
  let captured = read_capture("querypathinfo_request.bin");
  let (path, _) = parse_string(&captured, 8);

  let mut buf = Vec::new();
  let mut w = Writer::new(&mut buf);
  write_query_path_info_request(&mut w, &path).unwrap();

  compare_buffers(&buf, &captured, "querypathinfo_request");
}

#[test]
fn test_queryreferrers_request() {
  let captured = read_capture("queryreferrers_request.bin");
  let (path, _) = parse_string(&captured, 8);

  let mut buf = Vec::new();
  let mut w = Writer::new(&mut buf);
  write_query_referrers_request(&mut w, &path).unwrap();

  compare_buffers(&buf, &captured, "queryreferrers_request");
}

#[test]
fn test_addtemproot_request() {
  let captured = read_capture("addtemproot_request.bin");
  let (path, _) = parse_string(&captured, 8);

  let mut buf = Vec::new();
  let mut w = Writer::new(&mut buf);
  write_add_temp_root_request(&mut w, &path).unwrap();

  compare_buffers(&buf, &captured, "addtemproot_request");
}

#[test]
fn test_addindirectroot_request() {
  let captured = read_capture("addindirectroot_request.bin");
  let (path, _) = parse_string(&captured, 8);

  let mut buf = Vec::new();
  let mut w = Writer::new(&mut buf);
  write_add_indirect_root_request(&mut w, &path).unwrap();

  compare_buffers(&buf, &captured, "addindirectroot_request");
}

#[test]
fn test_findroots_request() {
  let mut buf = Vec::new();
  let mut w = Writer::new(&mut buf);
  write_find_roots_request(&mut w).unwrap();

  let expected = read_capture("findroots_request.bin");
  compare_buffers(&buf, &expected, "findroots_request");
}

#[test]
fn test_narfrompath_request() {
  let captured = read_capture("narfrompath_request.bin");
  let (path, _) = parse_string(&captured, 8);

  let mut buf = Vec::new();
  let mut w = Writer::new(&mut buf);
  write_nar_from_path_request(&mut w, &path).unwrap();

  compare_buffers(&buf, &captured, "narfrompath_request");
}

#[test]
fn test_querymissing_request() {
  let captured = read_capture("querymissing_request.bin");

  // Parse: op(8) + num_paths(8) + paths...
  let num_paths = u64::from_le_bytes(captured[8..16].try_into().unwrap()) as usize;
  let mut paths = Vec::new();
  let mut offset = 16;

  for _ in 0..num_paths {
    let (path, new_offset) = parse_string(&captured, offset);
    paths.push(path);
    offset = new_offset;
  }

  let path_refs: Vec<&str> = paths.iter().map(|s| s.as_str()).collect();

  let mut buf = Vec::new();
  let mut w = Writer::new(&mut buf);
  write_query_missing_request(&mut w, &path_refs).unwrap();

  compare_buffers(&buf, &captured, "querymissing_request");
}

#[test]
fn test_buildpaths_request() {
  let captured = read_capture("buildpaths_request.bin");

  // Parse: op(8) + num_paths(8) + paths... + mode(8)
  let num_paths = u64::from_le_bytes(captured[8..16].try_into().unwrap()) as usize;
  let mut paths = Vec::new();
  let mut offset = 16;

  for _ in 0..num_paths {
    let (path, new_offset) = parse_string(&captured, offset);
    paths.push(path);
    offset = new_offset;
  }

  let mode_val = u64::from_le_bytes(captured[offset..offset + 8].try_into().unwrap());
  let mode = match mode_val {
    0 => BuildMode::Normal,
    1 => BuildMode::Repair,
    2 => BuildMode::Check,
    _ => panic!("Unknown build mode: {}", mode_val),
  };

  let path_refs: Vec<&str> = paths.iter().map(|s| s.as_str()).collect();

  let mut buf = Vec::new();
  let mut w = Writer::new(&mut buf);
  write_build_paths_request(&mut w, &path_refs, mode).unwrap();

  compare_buffers(&buf, &captured, "buildpaths_request");
}

#[test]
fn test_buildpathswithresults_request() {
  let captured = read_capture("buildpathswithresults_request.bin");

  // Parse: op(8) + num_paths(8) + paths... + mode(8)
  let num_paths = u64::from_le_bytes(captured[8..16].try_into().unwrap()) as usize;
  let mut paths = Vec::new();
  let mut offset = 16;

  for _ in 0..num_paths {
    let (path, new_offset) = parse_string(&captured, offset);
    paths.push(path);
    offset = new_offset;
  }

  let mode_val = u64::from_le_bytes(captured[offset..offset + 8].try_into().unwrap());
  let mode = match mode_val {
    0 => BuildMode::Normal,
    1 => BuildMode::Repair,
    2 => BuildMode::Check,
    _ => panic!("Unknown build mode: {}", mode_val),
  };

  let path_refs: Vec<&str> = paths.iter().map(|s| s.as_str()).collect();

  let mut buf = Vec::new();
  let mut w = Writer::new(&mut buf);
  write_build_paths_with_results_request(&mut w, &path_refs, mode).unwrap();

  compare_buffers(&buf, &captured, "buildpathswithresults_request");
}

#[test]
fn test_setoptions_request() {
  // Hardcoded values from captured traffic
  let settings = ClientSettings {
    keep_failed: false,
    keep_going: false,
    try_fallback: false,
    verbosity: 3,
    max_build_jobs: 32,
    max_silent_time: 0,
    use_build_hook: true,
    verbose_build: 7,
    log_type: 0,
    print_build_trace: 0,
    build_cores: 0,
    use_substitutes: true,
    overrides: vec![
      ("extra-platforms".to_string(), "aarch64-linux".to_string()),
      ("sandbox".to_string(), "false".to_string()),
      (
        "substituters".to_string(),
        "https://cache.nixos.org https://weyl-ai.cachix.org".to_string(),
      ),
      (
        "trusted-public-keys".to_string(),
        "cache.nixos.org-1:6NCHdD59X431o0gWypbMrAURkbJ16ZPMQFGspcDShjY= \
         weyl-ai.cachix.org-1:cR0SpSAPw7wejZ21ep4SLojE77gp5F2os260eEWqTTw="
          .to_string(),
      ),
    ],
  };

  let mut buf = Vec::new();
  let mut w = Writer::new(&mut buf);
  write_set_options_request(&mut w, &settings, 38).unwrap();

  let expected = read_capture("setoptions_request.bin");
  compare_buffers(&buf, &expected, "setoptions_request");
}

#[test]
fn test_addtostorenar_request() {
  let req = AddToStoreNarRequest {
    path: "/nix/store/v4wqkf7dq9619yyv1wbf1jvpw4dhbs2f-tf.txt".to_string(),
    deriver: String::new(),
    nar_hash: "60e5106c2a1d4ef9f82b58df02be7a482f35438ff9e6556194ab02e355256352".to_string(),
    references: vec![],
    registration_time: 0,
    nar_size: 136,
    ultimate: false,
    signatures: vec![],
    ca: "fixed:sha256:081b9nklhcc0gck2a6j821xij57djvnfxg1fps0f5l2bbpd1sgp1".to_string(),
    repair: false,
    dont_check_sigs: false,
  };

  let mut buf = Vec::new();
  let mut w = Writer::new(&mut buf);
  write_add_to_store_nar_request(&mut w, &req).unwrap();

  let expected = read_capture("addtostorenar_request.bin");
  compare_buffers(&buf, &expected, "addtostorenar_request");
}

// =============================================================================
// Reader Tests
// =============================================================================

#[test]
fn test_read_server_hello() {
  let data = read_capture("server_hello.bin");
  let mut r = Reader::new(Cursor::new(&data));

  let hello = read_server_hello(&mut r).unwrap();
  assert_eq!(hello.magic, WORKER_MAGIC_2);
  assert_eq!(hello.version, 0x0126); // Protocol version 1.38
}

#[test]
fn test_read_isvalidpath_response() {
  let data = read_capture("isvalidpath_response.bin");
  let mut r = Reader::new(Cursor::new(&data));

  let valid = read_is_valid_path_response(&mut r).unwrap();
  assert!(valid);
}

#[test]
fn test_read_querypathinfo_response() {
  let data = read_capture("querypathinfo_response.bin");
  let mut r = Reader::new(Cursor::new(&data));

  let info = read_query_path_info_response(&mut r, 0x0126).unwrap();
  assert!(info.is_some());

  let info = info.unwrap();
  // Check deriver path
  assert!(info.deriver.contains("bash-5.3p9.drv"));
  // Check NAR hash (64 hex chars)
  assert_eq!(info.nar_hash.len(), 64);
  assert!(info.nar_hash.starts_with("f7b02ee0"));
  // Check references
  assert_eq!(info.references.len(), 2);
  // Check NAR size
  assert_eq!(info.nar_size, 0x1c5550); // 1856848 bytes
  // Check signature
  assert_eq!(info.signatures.len(), 1);
  assert!(info.signatures[0].starts_with("cache.nixos.org-1:"));
}

#[test]
fn test_read_querymissing_response() {
  let data = read_capture("querymissing_response.bin");
  let mut r = Reader::new(Cursor::new(&data));

  let result = read_query_missing_response(&mut r).unwrap();
  // All empty sets with zero sizes (nothing missing)
  assert!(result.will_build.is_empty());
  assert!(result.will_substitute.is_empty());
  assert!(result.unknown.is_empty());
  assert_eq!(result.download_size, 0);
  assert_eq!(result.nar_size, 0);
}

#[test]
fn test_read_queryreferrers_response() {
  let data = read_capture("queryreferrers_response.bin");
  let mut r = Reader::new(Cursor::new(&data));

  let referrers = read_query_referrers_response(&mut r).unwrap();
  // Should have some referrers
  assert!(!referrers.is_empty());
  // All should be store paths
  for path in &referrers {
    assert!(path.starts_with("/nix/store/"));
  }
}

#[test]
fn test_read_findroots_response() {
  // Note: This capture file is truncated (65KB limit), so we test partial parsing
  let data = read_capture("findroots_response.bin");

  // Manually parse some entries to verify format
  let mut offset = 0;
  let count = u64::from_le_bytes(data[offset..offset + 8].try_into().unwrap());
  assert_eq!(count, 10573); // Declared count
  offset += 8;

  // Parse first entry manually to verify format
  let (link, new_offset) = parse_string(&data, offset);
  assert!(link.starts_with("{temp:") || link.starts_with("/"));
  offset = new_offset;

  let (target, _) = parse_string(&data, offset);
  assert!(target.starts_with("/nix/store/"));
}

#[test]
fn test_read_buildpathswithresults_response() {
  let data = read_capture("buildpathswithresults_response.bin");
  let mut r = Reader::new(Cursor::new(&data));

  let results = read_build_paths_with_results_response(&mut r, 0x0126).unwrap();
  assert_eq!(results.len(), 1);

  let result = &results[0];
  // Check the derived path
  assert!(result.path.contains("hello"));
  assert!(result.path.ends_with("!out"));
  // Build succeeded (status 2 = "AlreadyValid" / "Built")
  assert_eq!(result.status, 2);
  // Should have built outputs
  assert_eq!(result.built_outputs.len(), 1);

  // Check realisation
  let (output_name, realisation) = &result.built_outputs[0];
  assert!(output_name.contains("sha256:"));
  assert!(realisation.id.contains("sha256:"));
  assert!(realisation.out_path.contains("hello"));
}

// =============================================================================
// Round-trip tests (write -> read)
// =============================================================================

#[test]
fn test_roundtrip_server_hello() {
  // Write
  let mut buf = Vec::new();
  let mut w = Writer::new(&mut buf);
  write_server_hello(&mut w, 0x0123).unwrap();

  // Read
  let mut r = Reader::new(Cursor::new(&buf));
  let hello = read_server_hello(&mut r).unwrap();

  assert_eq!(hello.magic, WORKER_MAGIC_2);
  assert_eq!(hello.version, 0x0123);
}

#[test]
fn test_roundtrip_query_path_info_response() {
  let info = ValidPathInfo {
    deriver: "/nix/store/abc-test.drv".to_string(),
    nar_hash: "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef".to_string(),
    references: vec![
      "/nix/store/aaa-ref1".to_string(),
      "/nix/store/bbb-ref2".to_string(),
    ],
    registration_time: 1234567890,
    nar_size: 12345,
    ultimate: true,
    signatures: vec!["cache:sig==".to_string()],
    ca: "".to_string(),
  };

  // Write
  let mut buf = Vec::new();
  let mut w = Writer::new(&mut buf);
  write_query_path_info_response(&mut w, Some(&info), 0x0126).unwrap();

  // Read (skip STDERR_LAST at beginning)
  let mut r = Reader::new(Cursor::new(&buf[8..])); // Skip STDERR_LAST
  let parsed = read_query_path_info_response(&mut r, 0x0126).unwrap();

  assert!(parsed.is_some());
  let parsed = parsed.unwrap();
  assert_eq!(parsed.deriver, info.deriver);
  assert_eq!(parsed.nar_hash, info.nar_hash);
  assert_eq!(parsed.references, info.references);
  assert_eq!(parsed.registration_time, info.registration_time);
  assert_eq!(parsed.nar_size, info.nar_size);
  assert_eq!(parsed.ultimate, info.ultimate);
  assert_eq!(parsed.signatures, info.signatures);
  assert_eq!(parsed.ca, info.ca);
}

#[test]
fn test_roundtrip_bool_response() {
  // Write
  let mut buf = Vec::new();
  let mut w = Writer::new(&mut buf);
  write_bool_response(&mut w, true).unwrap();

  // Read (skip STDERR_LAST at beginning)
  let mut r = Reader::new(Cursor::new(&buf[8..])); // Skip STDERR_LAST
  let value = read_is_valid_path_response(&mut r).unwrap();

  assert!(value);
}
