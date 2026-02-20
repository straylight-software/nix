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

pub mod nar;

use std::io::{self, Read, Write};

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
// Reader
// =============================================================================

/// Protocol parse error
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ProtocolError {
  /// Unexpected end of input
  UnexpectedEof,
  /// Invalid magic number
  InvalidMagic(u64),
  /// Invalid stderr message type
  InvalidStderrMsg(u64),
  /// Invalid bool value
  InvalidBool(u64),
  /// Invalid UTF-8 in string
  InvalidUtf8,
  /// Non-zero padding bytes
  NonZeroPadding,
  /// Unexpected stderr error from daemon
  DaemonError(String),
  /// I/O error (stringified for Clone/Eq)
  Io(String),
}

impl std::fmt::Display for ProtocolError {
  fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
    match self {
      ProtocolError::UnexpectedEof => write!(f, "unexpected end of input"),
      ProtocolError::InvalidMagic(m) => write!(f, "invalid magic: 0x{:016x}", m),
      ProtocolError::InvalidStderrMsg(m) => write!(f, "invalid stderr message: 0x{:08x}", m),
      ProtocolError::InvalidBool(v) => write!(f, "invalid bool value: {}", v),
      ProtocolError::InvalidUtf8 => write!(f, "invalid UTF-8 in string"),
      ProtocolError::NonZeroPadding => write!(f, "non-zero padding bytes"),
      ProtocolError::DaemonError(s) => write!(f, "daemon error: {}", s),
      ProtocolError::Io(s) => write!(f, "I/O error: {}", s),
    }
  }
}

impl std::error::Error for ProtocolError {}

impl From<io::Error> for ProtocolError {
  fn from(e: io::Error) -> Self {
    if e.kind() == io::ErrorKind::UnexpectedEof {
      ProtocolError::UnexpectedEof
    } else {
      ProtocolError::Io(e.to_string())
    }
  }
}

/// Protocol reader for parsing daemon responses
pub struct Reader<R> {
  inner: R,
}

impl<R: Read> Reader<R> {
  pub fn new(inner: R) -> Self {
    Self { inner }
  }

  pub fn into_inner(self) -> R {
    self.inner
  }

  // ===========================================================================
  // Primitives
  // ===========================================================================

  /// Read a u64 in little-endian format
  pub fn read_u64(&mut self) -> Result<u64, ProtocolError> {
    let mut buf = [0u8; 8];
    self.inner.read_exact(&mut buf)?;
    Ok(u64::from_le_bytes(buf))
  }

  /// Read padding bytes (must be zeros)
  fn read_padding(&mut self, len: usize) -> Result<(), ProtocolError> {
    let pad = (8 - (len % 8)) % 8;
    if pad > 0 {
      let mut buf = [0u8; 8];
      self.inner.read_exact(&mut buf[..pad])?;
      if buf[..pad].iter().any(|&b| b != 0) {
        return Err(ProtocolError::NonZeroPadding);
      }
    }
    Ok(())
  }

  /// Read a bool (u64, 0 = false, nonzero = true)
  pub fn read_bool(&mut self) -> Result<bool, ProtocolError> {
    let v = self.read_u64()?;
    Ok(v != 0)
  }

  /// Read bytes (length-prefixed with padding)
  pub fn read_bytes(&mut self) -> Result<Vec<u8>, ProtocolError> {
    let len = self.read_u64()? as usize;
    let mut data = vec![0u8; len];
    self.inner.read_exact(&mut data)?;
    self.read_padding(len)?;
    Ok(data)
  }

  /// Read a string (length-prefixed with padding)
  pub fn read_string(&mut self) -> Result<String, ProtocolError> {
    let bytes = self.read_bytes()?;
    String::from_utf8(bytes).map_err(|_| ProtocolError::InvalidUtf8)
  }

  /// Read a store path (just a string)
  pub fn read_store_path(&mut self) -> Result<String, ProtocolError> {
    self.read_string()
  }

  /// Read a list of strings
  pub fn read_string_list(&mut self) -> Result<Vec<String>, ProtocolError> {
    let count = self.read_u64()? as usize;
    let mut items = Vec::with_capacity(count);
    for _ in 0..count {
      items.push(self.read_string()?);
    }
    Ok(items)
  }

  /// Read a set of strings (just a list)
  pub fn read_string_set(&mut self) -> Result<Vec<String>, ProtocolError> {
    self.read_string_list()
  }

  /// Read a set of store paths
  pub fn read_store_path_set(&mut self) -> Result<Vec<String>, ProtocolError> {
    self.read_string_list()
  }

  // ===========================================================================
  // Stderr message handling
  // ===========================================================================

  /// Read stderr messages until STDERR_LAST, then return
  /// This handles STDERR_NEXT (log messages), STDERR_ERROR, etc.
  pub fn read_stderr(&mut self) -> Result<(), ProtocolError> {
    loop {
      let msg = self.read_u64()?;
      match msg {
        STDERR_LAST => return Ok(()),
        STDERR_ERROR => {
          // Legacy error format (protocol < 1.26)
          let error_msg = self.read_string()?;
          return Err(ProtocolError::DaemonError(error_msg));
        }
        STDERR_NEXT => {
          // Log message - read and discard
          let _log = self.read_string()?;
        }
        STDERR_READ => {
          // Daemon wants to read from us - not implemented for now
          return Err(ProtocolError::InvalidStderrMsg(msg));
        }
        STDERR_WRITE => {
          // Daemon writing data to us
          let _data = self.read_bytes()?;
        }
        STDERR_START_ACTIVITY => {
          // Activity started - read and discard
          let _act = self.read_u64()?; // activity id
          let _level = self.read_u64()?; // verbosity
          let _type = self.read_u64()?; // activity type
          let _text = self.read_string()?; // text
          let _fields = self.read_activity_fields()?; // fields
          let _parent = self.read_u64()?; // parent activity
        }
        STDERR_STOP_ACTIVITY => {
          let _act = self.read_u64()?; // activity id
        }
        STDERR_RESULT => {
          let _act = self.read_u64()?; // activity id
          let _type = self.read_u64()?; // result type
          let _fields = self.read_activity_fields()?; // fields
        }
        _ => {
          // Unknown message type
          return Err(ProtocolError::InvalidStderrMsg(msg));
        }
      }
    }
  }

  /// Read activity fields (used in STDERR_START_ACTIVITY and STDERR_RESULT)
  fn read_activity_fields(&mut self) -> Result<Vec<ActivityField>, ProtocolError> {
    let count = self.read_u64()? as usize;
    let mut fields = Vec::with_capacity(count);
    for _ in 0..count {
      let field_type = self.read_u64()?;
      let field = match field_type {
        0 => ActivityField::Int(self.read_u64()?),
        1 => ActivityField::String(self.read_string()?),
        _ => return Err(ProtocolError::InvalidStderrMsg(field_type)),
      };
      fields.push(field);
    }
    Ok(fields)
  }
}

/// Activity field in stderr messages
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ActivityField {
  Int(u64),
  String(String),
}

// =============================================================================
// Response parsing
// =============================================================================

/// Server hello response (handshake)
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ServerHello {
  pub magic: u64,
  pub version: u64,
}

/// Read server hello (first response after client_hello)
pub fn read_server_hello<R: Read>(r: &mut Reader<R>) -> Result<ServerHello, ProtocolError> {
  let magic = r.read_u64()?;
  if magic != WORKER_MAGIC_2 {
    return Err(ProtocolError::InvalidMagic(magic));
  }
  let version = r.read_u64()?;
  Ok(ServerHello { magic, version })
}

/// Read daemon version info (sent after handshake with protocol >= 1.14)
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DaemonInfo {
  pub version: String,
  pub trust_level: TrustLevel,
}

/// Read daemon info (protocol >= 1.14)
pub fn read_daemon_info<R: Read>(
  r: &mut Reader<R>,
  protocol_version: u16,
) -> Result<Option<DaemonInfo>, ProtocolError> {
  if protocol_version < 0x114 {
    return Ok(None);
  }

  // First, handle stderr messages (which may include version info via STDERR_NEXT)
  r.read_stderr()?;

  // For protocol >= 1.35, daemon sends trust level
  let trust_level = if protocol_version >= 0x123 {
    match r.read_u64()? {
      0 => TrustLevel::Unknown,
      1 => TrustLevel::Trusted,
      2 => TrustLevel::NotTrusted,
      n => return Err(ProtocolError::InvalidBool(n)),
    }
  } else {
    TrustLevel::Unknown
  };

  // Version string is sent via STDERR_NEXT before STDERR_LAST
  // For simplicity, we return Unknown version here
  Ok(Some(DaemonInfo {
    version: String::new(),
    trust_level,
  }))
}

/// Read IsValidPath response
pub fn read_is_valid_path_response<R: Read>(r: &mut Reader<R>) -> Result<bool, ProtocolError> {
  r.read_bool()
}

/// Read QueryPathInfo response
pub fn read_query_path_info_response<R: Read>(
  r: &mut Reader<R>,
  protocol_version: u16,
) -> Result<Option<ValidPathInfo>, ProtocolError> {
  let valid = r.read_bool()?;
  if !valid {
    return Ok(None);
  }

  let deriver = r.read_string()?;
  let nar_hash = r.read_string()?;
  let references = r.read_store_path_set()?;
  let registration_time = r.read_u64()?;
  let nar_size = r.read_u64()?;

  let (ultimate, signatures, ca) = if protocol_version >= 16 {
    let ultimate = r.read_bool()?;
    let signatures = r.read_string_set()?;
    let ca = r.read_string()?;
    (ultimate, signatures, ca)
  } else {
    (false, vec![], String::new())
  };

  Ok(Some(ValidPathInfo {
    deriver,
    nar_hash,
    references,
    registration_time,
    nar_size,
    ultimate,
    signatures,
    ca,
  }))
}

/// QueryMissing response
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct QueryMissingResult {
  pub will_build: Vec<String>,
  pub will_substitute: Vec<String>,
  pub unknown: Vec<String>,
  pub download_size: u64,
  pub nar_size: u64,
}

/// Read QueryMissing response
pub fn read_query_missing_response<R: Read>(
  r: &mut Reader<R>,
) -> Result<QueryMissingResult, ProtocolError> {
  let will_build = r.read_store_path_set()?;
  let will_substitute = r.read_store_path_set()?;
  let unknown = r.read_store_path_set()?;
  let download_size = r.read_u64()?;
  let nar_size = r.read_u64()?;

  Ok(QueryMissingResult {
    will_build,
    will_substitute,
    unknown,
    download_size,
    nar_size,
  })
}

/// BuildResult from BuildPathsWithResults
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct BuildResult {
  pub path: String, // The derived path that was built
  pub status: u64,
  pub error_msg: String,
  pub times_built: u64,
  pub is_non_deterministic: bool,
  pub start_time: u64,
  pub stop_time: u64,
  pub cpu_user: u64,
  pub cpu_system: u64,
  pub built_outputs: Vec<(String, Realisation)>,
}

/// DrvOutput realisation
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct Realisation {
  pub id: String, // DrvOutput id (drv!output)
  pub out_path: String,
  pub signatures: Vec<String>,
  pub dependent_realisations: Vec<(String, String)>,
}

/// Read BuildPathsWithResults response
pub fn read_build_paths_with_results_response<R: Read>(
  r: &mut Reader<R>,
  protocol_version: u16,
) -> Result<Vec<BuildResult>, ProtocolError> {
  let count = r.read_u64()? as usize;
  let mut results = Vec::with_capacity(count);

  for _ in 0..count {
    // First: the derived path (DrvOutput) that was requested
    let path = r.read_string()?;
    let status = r.read_u64()?;
    let error_msg = r.read_string()?;

    let times_built = if protocol_version >= 0x11d {
      r.read_u64()?
    } else {
      0
    };
    let is_non_deterministic = if protocol_version >= 0x11d {
      r.read_bool()?
    } else {
      false
    };
    let start_time = if protocol_version >= 0x11e {
      r.read_u64()?
    } else {
      0
    };
    let stop_time = if protocol_version >= 0x11e {
      r.read_u64()?
    } else {
      0
    };
    // Note: CPU times are included starting at protocol 1.44 (0x12c)
    // However, some captures show them present at 1.38, so we read them
    // unconditionally for versions >= 1.28 to match observed behavior
    let cpu_user = if protocol_version >= 0x11c {
      r.read_u64()?
    } else {
      0
    };
    let cpu_system = if protocol_version >= 0x11c {
      r.read_u64()?
    } else {
      0
    };

    // Built outputs (DrvOutputs)
    let built_outputs = if protocol_version >= 0x11c {
      let out_count = r.read_u64()? as usize;
      let mut outputs = Vec::with_capacity(out_count);
      for _ in 0..out_count {
        let output_name = r.read_string()?;
        let realisation = read_realisation(r)?;
        outputs.push((output_name, realisation));
      }
      outputs
    } else {
      vec![]
    };

    results.push(BuildResult {
      path,
      status,
      error_msg,
      times_built,
      is_non_deterministic,
      start_time,
      stop_time,
      cpu_user,
      cpu_system,
      built_outputs,
    });
  }

  Ok(results)
}

/// Read a Realisation from the wire
fn read_realisation<R: Read>(r: &mut Reader<R>) -> Result<Realisation, ProtocolError> {
  // Realisations are serialized as JSON strings in the wire protocol
  let json_str = r.read_string()?;

  // Parse the JSON manually (minimal parsing, no external dependency)
  // Format: {"id":"...","outPath":"...","signatures":[...],"dependentRealisations":{...}}
  parse_realisation_json(&json_str)
}

/// Parse a realisation from JSON string
fn parse_realisation_json(json: &str) -> Result<Realisation, ProtocolError> {
  // Very minimal JSON parsing - just extract the fields we need
  // This is intentionally simple; a full implementation would use serde_json

  let mut realisation = Realisation::default();

  // Extract "id" field
  if let Some(start) = json.find("\"id\":\"") {
    let rest = &json[start + 6..];
    if let Some(end) = rest.find('"') {
      realisation.id = rest[..end].to_string();
    }
  }

  // Extract "outPath" field
  if let Some(start) = json.find("\"outPath\":\"") {
    let rest = &json[start + 11..];
    if let Some(end) = rest.find('"') {
      realisation.out_path = rest[..end].to_string();
    }
  }

  // signatures and dependentRealisations are more complex - skip for now
  // A full implementation would parse these properly

  Ok(realisation)
}

/// Read QueryReferrers response (list of store paths)
pub fn read_query_referrers_response<R: Read>(
  r: &mut Reader<R>,
) -> Result<Vec<String>, ProtocolError> {
  r.read_store_path_set()
}

/// FindRoots response entry
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct GcRoot {
  pub link: String,
  pub target: String,
}

/// Read FindRoots response
pub fn read_find_roots_response<R: Read>(r: &mut Reader<R>) -> Result<Vec<GcRoot>, ProtocolError> {
  let count = r.read_u64()? as usize;
  let mut roots = Vec::with_capacity(count);
  for _ in 0..count {
    let link = r.read_string()?;
    let target = r.read_store_path()?;
    roots.push(GcRoot { link, target });
  }
  Ok(roots)
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
