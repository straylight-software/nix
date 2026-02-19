// SPDX-License-Identifier: MIT
//! Nix Daemon Protocol Serializers
//!
//! Wire format primitives:
//! - Integers: little-endian u64
//! - Strings: u64 length + bytes + padding to 8-byte boundary
//! - Booleans: u64 (0 = false, nonzero = true)
//! - Lists: u64 count + elements
//!
//! # Example
//! ```
//! use nix_protocol::{Writer, write_client_hello, write_query_path_info_request};
//!
//! let mut buf = Vec::new();
//! let mut w = Writer::new(&mut buf);
//! write_client_hello(&mut w, 0x0126); // version 1.38
//! write_query_path_info_request(&mut w, "/nix/store/...");
//! ```

use std::io::{self, Write};

// =============================================================================
// Protocol Constants
// =============================================================================

pub const WORKER_MAGIC_1: u64 = 0x6e697863; // "nixc" client->server
pub const WORKER_MAGIC_2: u64 = 0x6478696f; // "dxio" server->client

pub const STDERR_NEXT: u64 = 0x6f6c6d67;
pub const STDERR_READ: u64 = 0x64617461;
pub const STDERR_WRITE: u64 = 0x64617416;
pub const STDERR_LAST: u64 = 0x616c7473;
pub const STDERR_ERROR: u64 = 0x63787470;
pub const STDERR_START_ACTIVITY: u64 = 0x53545254;
pub const STDERR_STOP_ACTIVITY: u64 = 0x53544f50;
pub const STDERR_RESULT: u64 = 0x52534c54;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u64)]
pub enum Op {
  IsValidPath = 1,
  HasSubstitutes = 3,
  QueryReferrers = 6,
  AddToStore = 7,
  BuildPaths = 9,
  EnsurePath = 10,
  AddTempRoot = 11,
  AddIndirectRoot = 12,
  SyncWithGC = 13,
  FindRoots = 14,
  SetOptions = 19,
  CollectGarbage = 20,
  QuerySubstitutablePathInfo = 21,
  QueryAllValidPaths = 23,
  QueryPathInfo = 26,
  QueryPathFromHashPart = 29,
  QuerySubstitutablePathInfos = 30,
  QueryValidPaths = 31,
  QuerySubstitutablePaths = 32,
  QueryValidDerivers = 33,
  OptimiseStore = 34,
  VerifyStore = 35,
  BuildDerivation = 36,
  AddSignatures = 37,
  NarFromPath = 38,
  AddToStoreNar = 39,
  QueryMissing = 40,
  QueryDerivationOutputMap = 41,
  RegisterDrvOutput = 42,
  QueryRealisation = 43,
  AddMultipleToStore = 44,
  AddBuildLog = 45,
  BuildPathsWithResults = 46,
  AddPermRoot = 47,
  QueryActiveBuilds = 48,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
#[repr(u64)]
pub enum BuildMode {
  #[default]
  Normal = 0,
  Repair = 1,
  Check = 2,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u64)]
pub enum TrustLevel {
  Unknown = 0,
  Trusted = 1,
  NotTrusted = 2,
}

// =============================================================================
// Writer
// =============================================================================

pub struct Writer<W> {
  inner: W,
}

impl<W: Write> Writer<W> {
  pub fn new(inner: W) -> Self {
    Self { inner }
  }

  pub fn into_inner(self) -> W {
    self.inner
  }

  // Primitives

  pub fn write_u64(&mut self, val: u64) -> io::Result<()> {
    self.inner.write_all(&val.to_le_bytes())
  }

  pub fn write_bool(&mut self, val: bool) -> io::Result<()> {
    self.write_u64(if val { 1 } else { 0 })
  }

  pub fn write_bytes(&mut self, data: &[u8]) -> io::Result<()> {
    self.write_u64(data.len() as u64)?;
    self.inner.write_all(data)?;
    self.write_padding(data.len())
  }

  pub fn write_string(&mut self, s: &str) -> io::Result<()> {
    self.write_bytes(s.as_bytes())
  }

  fn write_padding(&mut self, len: usize) -> io::Result<()> {
    let pad = (8 - (len % 8)) % 8;
    if pad > 0 {
      self.inner.write_all(&[0u8; 8][..pad])?;
    }
    Ok(())
  }

  // Composites

  pub fn write_string_list(&mut self, items: &[&str]) -> io::Result<()> {
    self.write_u64(items.len() as u64)?;
    for s in items {
      self.write_string(s)?;
    }
    Ok(())
  }

  pub fn write_string_list_owned(&mut self, items: &[String]) -> io::Result<()> {
    self.write_u64(items.len() as u64)?;
    for s in items {
      self.write_string(s)?;
    }
    Ok(())
  }

  pub fn write_string_set(&mut self, items: &[&str]) -> io::Result<()> {
    let mut sorted: Vec<&str> = items.to_vec();
    sorted.sort();
    self.write_u64(sorted.len() as u64)?;
    for s in sorted {
      self.write_string(s)?;
    }
    Ok(())
  }

  pub fn write_string_set_owned(&mut self, items: &[String]) -> io::Result<()> {
    let mut sorted: Vec<&String> = items.iter().collect();
    sorted.sort();
    self.write_u64(sorted.len() as u64)?;
    for s in sorted {
      self.write_string(s)?;
    }
    Ok(())
  }

  pub fn write_store_path(&mut self, path: &str) -> io::Result<()> {
    self.write_string(path)
  }

  pub fn write_store_path_set(&mut self, paths: &[&str]) -> io::Result<()> {
    self.write_string_set(paths)
  }

  pub fn write_store_path_set_owned(&mut self, paths: &[String]) -> io::Result<()> {
    self.write_string_set_owned(paths)
  }

  pub fn write_derived_path(&mut self, path: &str) -> io::Result<()> {
    self.write_string(path)
  }

  pub fn write_derived_path_list(&mut self, paths: &[&str]) -> io::Result<()> {
    self.write_u64(paths.len() as u64)?;
    for p in paths {
      self.write_derived_path(p)?;
    }
    Ok(())
  }

  pub fn write_derived_path_list_owned(&mut self, paths: &[String]) -> io::Result<()> {
    self.write_u64(paths.len() as u64)?;
    for p in paths {
      self.write_derived_path(p)?;
    }
    Ok(())
  }

  // Framed data (for NAR streaming, protocol >= 1.23)

  pub fn write_framed_data(&mut self, data: &[u8]) -> io::Result<()> {
    self.write_u64(data.len() as u64)?;
    self.inner.write_all(data)
  }

  pub fn write_framed_end(&mut self) -> io::Result<()> {
    self.write_u64(0)
  }

  // Raw write

  pub fn write_raw(&mut self, data: &[u8]) -> io::Result<()> {
    self.inner.write_all(data)
  }
}

// =============================================================================
// Handshake
// =============================================================================

pub fn write_client_hello<W: Write>(w: &mut Writer<W>, version: u64) -> io::Result<()> {
  w.write_u64(WORKER_MAGIC_1)?;
  w.write_u64(version)
}

pub fn write_server_hello<W: Write>(w: &mut Writer<W>, version: u64) -> io::Result<()> {
  w.write_u64(WORKER_MAGIC_2)?;
  w.write_u64(version)
}

// =============================================================================
// Requests
// =============================================================================

fn write_op<W: Write>(w: &mut Writer<W>, op: Op) -> io::Result<()> {
  w.write_u64(op as u64)
}

pub fn write_is_valid_path_request<W: Write>(w: &mut Writer<W>, path: &str) -> io::Result<()> {
  write_op(w, Op::IsValidPath)?;
  w.write_store_path(path)
}

pub fn write_query_path_info_request<W: Write>(w: &mut Writer<W>, path: &str) -> io::Result<()> {
  write_op(w, Op::QueryPathInfo)?;
  w.write_store_path(path)
}

pub fn write_query_referrers_request<W: Write>(w: &mut Writer<W>, path: &str) -> io::Result<()> {
  write_op(w, Op::QueryReferrers)?;
  w.write_store_path(path)
}

pub fn write_add_temp_root_request<W: Write>(w: &mut Writer<W>, path: &str) -> io::Result<()> {
  write_op(w, Op::AddTempRoot)?;
  w.write_store_path(path)
}

pub fn write_add_indirect_root_request<W: Write>(w: &mut Writer<W>, path: &str) -> io::Result<()> {
  write_op(w, Op::AddIndirectRoot)?;
  w.write_string(path)
}

pub fn write_find_roots_request<W: Write>(w: &mut Writer<W>) -> io::Result<()> {
  write_op(w, Op::FindRoots)
}

pub fn write_nar_from_path_request<W: Write>(w: &mut Writer<W>, path: &str) -> io::Result<()> {
  write_op(w, Op::NarFromPath)?;
  w.write_store_path(path)
}

pub fn write_query_missing_request<W: Write>(
  w: &mut Writer<W>,
  targets: &[&str],
) -> io::Result<()> {
  write_op(w, Op::QueryMissing)?;
  w.write_derived_path_list(targets)
}

pub fn write_query_missing_request_owned<W: Write>(
  w: &mut Writer<W>,
  targets: &[String],
) -> io::Result<()> {
  write_op(w, Op::QueryMissing)?;
  w.write_derived_path_list_owned(targets)
}

pub fn write_build_paths_request<W: Write>(
  w: &mut Writer<W>,
  paths: &[&str],
  mode: BuildMode,
) -> io::Result<()> {
  write_op(w, Op::BuildPaths)?;
  w.write_derived_path_list(paths)?;
  w.write_u64(mode as u64)
}

pub fn write_build_paths_request_owned<W: Write>(
  w: &mut Writer<W>,
  paths: &[String],
  mode: BuildMode,
) -> io::Result<()> {
  write_op(w, Op::BuildPaths)?;
  w.write_derived_path_list_owned(paths)?;
  w.write_u64(mode as u64)
}

pub fn write_build_paths_with_results_request<W: Write>(
  w: &mut Writer<W>,
  paths: &[&str],
  mode: BuildMode,
) -> io::Result<()> {
  write_op(w, Op::BuildPathsWithResults)?;
  w.write_derived_path_list(paths)?;
  w.write_u64(mode as u64)
}

pub fn write_build_paths_with_results_request_owned<W: Write>(
  w: &mut Writer<W>,
  paths: &[String],
  mode: BuildMode,
) -> io::Result<()> {
  write_op(w, Op::BuildPathsWithResults)?;
  w.write_derived_path_list_owned(paths)?;
  w.write_u64(mode as u64)
}

// SetOptions

#[derive(Debug, Clone, Default)]
pub struct ClientSettings {
  pub keep_failed: bool,
  pub keep_going: bool,
  pub try_fallback: bool,
  pub verbosity: u64,
  pub max_build_jobs: u64,
  pub max_silent_time: u64,
  pub use_build_hook: bool,
  pub verbose_build: u64,
  pub log_type: u64,
  pub print_build_trace: u64,
  pub build_cores: u64,
  pub use_substitutes: bool,
  pub overrides: Vec<(String, String)>,
}

pub fn write_set_options_request<W: Write>(
  w: &mut Writer<W>,
  settings: &ClientSettings,
  protocol_version: u16,
) -> io::Result<()> {
  write_op(w, Op::SetOptions)?;
  w.write_bool(settings.keep_failed)?;
  w.write_bool(settings.keep_going)?;
  w.write_bool(settings.try_fallback)?;
  w.write_u64(settings.verbosity)?;
  w.write_u64(settings.max_build_jobs)?;
  w.write_u64(settings.max_silent_time)?;
  w.write_bool(settings.use_build_hook)?;
  w.write_u64(settings.verbose_build)?;
  w.write_u64(settings.log_type)?;
  w.write_u64(settings.print_build_trace)?;
  w.write_u64(settings.build_cores)?;
  w.write_bool(settings.use_substitutes)?;

  if protocol_version >= 12 {
    w.write_u64(settings.overrides.len() as u64)?;
    for (name, value) in &settings.overrides {
      w.write_string(name)?;
      w.write_string(value)?;
    }
  }
  Ok(())
}

// AddToStoreNar

#[derive(Debug, Clone, Default)]
pub struct AddToStoreNarRequest {
  pub path: String,
  pub deriver: String, // empty = none
  pub nar_hash: String,
  pub references: Vec<String>,
  pub registration_time: u64,
  pub nar_size: u64,
  pub ultimate: bool,
  pub signatures: Vec<String>,
  pub ca: String, // empty = none
  pub repair: bool,
  pub dont_check_sigs: bool,
}

pub fn write_add_to_store_nar_request<W: Write>(
  w: &mut Writer<W>,
  req: &AddToStoreNarRequest,
) -> io::Result<()> {
  write_op(w, Op::AddToStoreNar)?;
  w.write_store_path(&req.path)?;
  w.write_string(&req.deriver)?; // optional, empty = none
  w.write_string(&req.nar_hash)?;
  w.write_store_path_set_owned(&req.references)?;
  w.write_u64(req.registration_time)?;
  w.write_u64(req.nar_size)?;
  w.write_bool(req.ultimate)?;
  w.write_string_set_owned(&req.signatures)?;
  w.write_string(&req.ca)?;
  w.write_bool(req.repair)?;
  w.write_bool(req.dont_check_sigs)
}

// =============================================================================
// Responses (for daemon implementation)
// =============================================================================

pub fn write_stderr_last<W: Write>(w: &mut Writer<W>) -> io::Result<()> {
  w.write_u64(STDERR_LAST)
}

pub fn write_stderr_error<W: Write>(w: &mut Writer<W>, msg: &str) -> io::Result<()> {
  w.write_u64(STDERR_ERROR)?;
  w.write_string(msg)
}

pub fn write_bool_response<W: Write>(w: &mut Writer<W>, value: bool) -> io::Result<()> {
  write_stderr_last(w)?;
  w.write_bool(value)
}

pub fn write_store_path_response<W: Write>(w: &mut Writer<W>, path: &str) -> io::Result<()> {
  write_stderr_last(w)?;
  w.write_store_path(path)
}

#[derive(Debug, Clone, Default)]
pub struct ValidPathInfo {
  pub deriver: String, // empty = none
  pub nar_hash: String,
  pub references: Vec<String>,
  pub registration_time: u64,
  pub nar_size: u64,
  pub ultimate: bool,
  pub signatures: Vec<String>,
  pub ca: String, // empty = none
}

pub fn write_query_path_info_response<W: Write>(
  w: &mut Writer<W>,
  info: Option<&ValidPathInfo>,
  protocol_version: u16,
) -> io::Result<()> {
  write_stderr_last(w)?;
  match info {
    None => w.write_bool(false),
    Some(info) => {
      w.write_bool(true)?;
      w.write_string(&info.deriver)?;
      w.write_string(&info.nar_hash)?;
      w.write_store_path_set_owned(&info.references)?;
      w.write_u64(info.registration_time)?;
      w.write_u64(info.nar_size)?;
      if protocol_version >= 16 {
        w.write_bool(info.ultimate)?;
        w.write_string_set_owned(&info.signatures)?;
        w.write_string(&info.ca)?;
      }
      Ok(())
    }
  }
}

// =============================================================================
// Tests
// =============================================================================

#[cfg(test)]
mod tests {
  use super::*;

  #[test]
  fn test_write_u64() {
    let mut buf = Vec::new();
    let mut w = Writer::new(&mut buf);
    w.write_u64(0x0126).unwrap();
    assert_eq!(buf, vec![0x26, 0x01, 0, 0, 0, 0, 0, 0]);
  }

  #[test]
  fn test_write_string() {
    let mut buf = Vec::new();
    let mut w = Writer::new(&mut buf);
    w.write_string("hello").unwrap();
    // len=5, "hello", padding=3
    assert_eq!(
      buf,
      vec![
        5, 0, 0, 0, 0, 0, 0, 0, b'h', b'e', b'l', b'l', b'o', 0, 0, 0
      ]
    );
  }

  #[test]
  fn test_client_hello() {
    let mut buf = Vec::new();
    let mut w = Writer::new(&mut buf);
    write_client_hello(&mut w, 0x0126).unwrap();
    assert_eq!(buf.len(), 16);
    // WORKER_MAGIC_1 = 0x6e697863
    assert_eq!(&buf[0..8], &[0x63, 0x78, 0x69, 0x6e, 0, 0, 0, 0]);
    // version 1.38 = 0x0126
    assert_eq!(&buf[8..16], &[0x26, 0x01, 0, 0, 0, 0, 0, 0]);
  }
}
