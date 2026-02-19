// SPDX-License-Identifier: MIT
//! Test Rust serializers against captured binary data

use nix_protocol::*;
use std::fs;
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
