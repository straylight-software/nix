meta:
  id: nix_daemon_protocol
  title: Nix Daemon Worker Protocol
  endian: le
  license: MIT

doc: |
  The Nix daemon protocol ("worker protocol") is used for communication
  between nix CLI tools and nix-daemon over unix sockets or SSH.
  
  Wire format primitives:
  - Integers: little-endian u64
  - Strings: u64 length + bytes + padding to 8-byte boundary
  - Booleans: u64 (0 = false, nonzero = true)
  - Lists: u64 count + elements
  - Sets: same as lists (sorted)
  - Optional<T>: empty string = None for string-like, tag byte for others
  
  Protocol version: 1.38 (current), minimum 1.10

params:
  - id: protocol_version
    type: u2
    doc: Negotiated protocol minor version (e.g., 38 for 1.38)

types:
  # ==========================================================================
  # Primitives
  # ==========================================================================
  
  nix_string:
    doc: Length-prefixed string, padded to 8-byte boundary
    seq:
      - id: len
        type: u8
      - id: data
        type: str
        encoding: UTF-8
        size: len
      - id: padding
        size: (8 - (len % 8)) % 8
  
  nix_bytes:
    doc: Length-prefixed bytes, padded to 8-byte boundary
    seq:
      - id: len
        type: u8
      - id: data
        size: len
      - id: padding
        size: (8 - (len % 8)) % 8

  nix_string_list:
    seq:
      - id: num_items
        type: u8
      - id: items
        type: nix_string
        repeat: expr
        repeat-expr: num_items

  nix_string_set:
    doc: Sorted set of strings (same wire format as list)
    seq:
      - id: num_items
        type: u8
      - id: items
        type: nix_string
        repeat: expr
        repeat-expr: num_items

  store_path:
    doc: |
      Store path, e.g. "/nix/store/abc123-hello-1.0"
    seq:
      - id: path
        type: nix_string

  store_path_set:
    seq:
      - id: num_paths
        type: u8
      - id: paths
        type: store_path
        repeat: expr
        repeat-expr: num_paths

  optional_store_path:
    doc: Empty string means None
    seq:
      - id: path
        type: nix_string
    instances:
      is_present:
        value: path.len > 0

  store_path_ca_map:
    doc: Map from StorePath to optional ContentAddress (protocol >= 1.22)
    seq:
      - id: num_entries
        type: u8
      - id: entries
        type: store_path_ca_entry
        repeat: expr
        repeat-expr: num_entries

  store_path_ca_entry:
    seq:
      - id: path
        type: store_path
      - id: ca
        type: nix_string
        doc: Content address string, empty = none

  # ==========================================================================
  # Handshake
  # ==========================================================================
  
  client_hello:
    doc: Initial message from client to daemon
    seq:
      - id: magic
        contents: [0x63, 0x78, 0x69, 0x6e]  # "cxin" = 0x6e697863 LE
        doc: WORKER_MAGIC_1
      - id: padding1
        contents: [0x00, 0x00, 0x00, 0x00]
      - id: client_version
        type: u8
        doc: Protocol version (major << 8 | minor)
  
  server_hello:
    doc: Response from daemon after receiving client_hello
    seq:
      - id: magic
        contents: [0x6f, 0x69, 0x78, 0x64]  # "oixd" = 0x6478696f LE  
        doc: WORKER_MAGIC_2
      - id: padding1
        contents: [0x00, 0x00, 0x00, 0x00]
      - id: server_version
        type: u8
        doc: Protocol version (major << 8 | minor)

  client_handshake_continuation:
    doc: |
      After version exchange, client sends additional handshake info.
      Fields depend on negotiated version.
    seq:
      - id: cpu_affinity_tag
        type: u8
        if: _root.protocol_version >= 14
      - id: cpu_affinity
        type: u8
        if: _root.protocol_version >= 14 and cpu_affinity_tag != 0
      - id: reserve_space
        type: u8
        if: _root.protocol_version >= 11

  server_handshake_info:
    doc: Server info sent after version negotiation
    seq:
      - id: daemon_version
        type: nix_string
        if: _root.protocol_version >= 33
      - id: trust_level
        type: u8
        if: _root.protocol_version >= 35
        enum: trust_level

  feature_exchange:
    doc: Protocol >= 1.38 feature negotiation
    seq:
      - id: features
        type: nix_string_set

  # ==========================================================================
  # Request Types
  # ==========================================================================
  
  request:
    doc: Client request to daemon
    seq:
      - id: op
        type: u8
        enum: operation
      - id: payload
        type:
          switch-on: op
          cases:
            'operation::is_valid_path': is_valid_path_request
            'operation::query_valid_paths': query_valid_paths_request
            'operation::has_substitutes': has_substitutes_request
            'operation::query_substitutable_paths': query_substitutable_paths_request
            'operation::query_path_hash': query_path_hash_request
            'operation::query_references': query_references_request
            'operation::query_referrers': query_referrers_request
            'operation::query_valid_derivers': query_valid_derivers_request
            'operation::query_derivation_outputs': query_derivation_outputs_request
            'operation::query_derivation_output_names': query_derivation_output_names_request
            'operation::query_derivation_output_map': query_derivation_output_map_request
            'operation::query_deriver': query_deriver_request
            'operation::query_path_from_hash_part': query_path_from_hash_part_request
            'operation::query_path_info': query_path_info_request
            'operation::query_all_valid_paths': empty_request
            'operation::add_to_store': add_to_store_request
            'operation::add_multiple_to_store': add_multiple_to_store_request
            'operation::add_text_to_store': add_text_to_store_request
            'operation::build_paths': build_paths_request
            'operation::build_paths_with_results': build_paths_with_results_request
            'operation::build_derivation': build_derivation_request
            'operation::ensure_path': ensure_path_request
            'operation::add_temp_root': add_temp_root_request
            'operation::add_perm_root': add_perm_root_request
            'operation::add_indirect_root': add_indirect_root_request
            'operation::sync_with_gc': empty_request
            'operation::find_roots': empty_request
            'operation::collect_garbage': collect_garbage_request
            'operation::set_options': set_options_request
            'operation::query_substitutable_path_info': query_substitutable_path_info_request
            'operation::query_substitutable_path_infos': query_substitutable_path_infos_request
            'operation::optimise_store': empty_request
            'operation::verify_store': verify_store_request
            'operation::add_signatures': add_signatures_request
            'operation::nar_from_path': nar_from_path_request
            'operation::add_to_store_nar': add_to_store_nar_request
            'operation::query_missing': query_missing_request
            'operation::register_drv_output': register_drv_output_request
            'operation::query_realisation': query_realisation_request
            'operation::add_build_log': add_build_log_request
            'operation::query_active_builds': empty_request

  empty_request:
    doc: Operations with no request payload
    seq: []

  # --- Simple path queries ---

  is_valid_path_request:
    seq:
      - id: path
        type: store_path

  has_substitutes_request:
    seq:
      - id: path
        type: store_path

  query_path_info_request:
    seq:
      - id: path
        type: store_path

  query_path_hash_request:
    seq:
      - id: path
        type: store_path

  query_references_request:
    seq:
      - id: path
        type: store_path

  query_referrers_request:
    seq:
      - id: path
        type: store_path

  query_valid_derivers_request:
    seq:
      - id: path
        type: store_path

  query_derivation_outputs_request:
    seq:
      - id: path
        type: store_path

  query_derivation_output_names_request:
    seq:
      - id: path
        type: store_path

  query_derivation_output_map_request:
    seq:
      - id: path
        type: store_path

  query_deriver_request:
    seq:
      - id: path
        type: store_path

  query_path_from_hash_part_request:
    seq:
      - id: hash_part
        type: nix_string
        doc: First 32 chars of store path hash

  # --- Multi-path queries ---

  query_valid_paths_request:
    seq:
      - id: paths
        type: store_path_set
      - id: substitute
        type: u8
        if: _root.protocol_version >= 27
        doc: Boolean - whether to substitute missing paths

  query_substitutable_paths_request:
    seq:
      - id: paths
        type: store_path_set

  query_substitutable_path_info_request:
    seq:
      - id: path
        type: store_path

  query_substitutable_path_infos_request:
    seq:
      - id: paths
        type: store_path_set
        if: _root.protocol_version < 22
      - id: paths_with_ca
        type: store_path_ca_map
        if: _root.protocol_version >= 22

  query_missing_request:
    seq:
      - id: targets
        type: derived_path_list

  # --- Store modification ---

  add_to_store_request:
    doc: |
      Add content to store. Protocol >= 1.25 uses new format.
      NAR data follows via framed source after request.
    seq:
      - id: name
        type: nix_string
        if: _root.protocol_version >= 25
      - id: ca_method
        type: nix_string
        if: _root.protocol_version >= 25
        doc: Content address method string (e.g. "nar:sha256")
      - id: refs
        type: store_path_set
        if: _root.protocol_version >= 25
      - id: repair
        type: u8
        if: _root.protocol_version >= 25
      # Old protocol (< 1.25)
      - id: base_name
        type: nix_string
        if: _root.protocol_version < 25
      - id: fixed
        type: u8
        if: _root.protocol_version < 25
      - id: recursive
        type: u1
        if: _root.protocol_version < 25
      - id: hash_algo
        type: nix_string
        if: _root.protocol_version < 25

  add_multiple_to_store_request:
    seq:
      - id: repair
        type: u8
      - id: dont_check_sigs
        type: u8

  add_text_to_store_request:
    doc: Obsolete since 1.25
    seq:
      - id: suffix
        type: nix_string
      - id: text
        type: nix_string
      - id: refs
        type: store_path_set

  add_to_store_nar_request:
    seq:
      - id: path
        type: store_path
      - id: deriver
        type: optional_store_path
      - id: nar_hash
        type: nix_string
        doc: SHA256 in base16
      - id: refs
        type: store_path_set
      - id: registration_time
        type: u8
      - id: nar_size
        type: u8
      - id: ultimate
        type: u8
      - id: sigs
        type: nix_string_set
      - id: ca
        type: nix_string
        doc: Content address, empty = none
      - id: repair
        type: u8
      - id: dont_check_sigs
        type: u8

  add_signatures_request:
    seq:
      - id: path
        type: store_path
      - id: sigs
        type: nix_string_set

  # --- Building ---

  build_paths_request:
    seq:
      - id: paths
        type: derived_path_list
      - id: build_mode
        type: u1
        enum: build_mode

  build_paths_with_results_request:
    seq:
      - id: paths
        type: derived_path_list
      - id: build_mode
        type: u1
        enum: build_mode

  build_derivation_request:
    doc: |
      Build a derivation. The derivation is sent inline (not read from store).
      Derivation format is ATerm-based, parsed separately.
    seq:
      - id: drv_path
        type: store_path
      - id: derivation
        type: nix_bytes
        doc: Serialized BasicDerivation in ATerm format
      - id: build_mode
        type: u1
        enum: build_mode

  ensure_path_request:
    seq:
      - id: path
        type: store_path

  # --- GC roots ---

  add_temp_root_request:
    seq:
      - id: path
        type: store_path

  add_perm_root_request:
    seq:
      - id: store_path
        type: store_path
      - id: gc_root
        type: nix_string
        doc: Absolute filesystem path for the symlink

  add_indirect_root_request:
    seq:
      - id: path
        type: nix_string
        doc: Absolute filesystem path (not store path)

  # --- GC ---

  collect_garbage_request:
    seq:
      - id: action
        type: u8
        enum: gc_action
      - id: paths_to_delete
        type: store_path_set
      - id: ignore_liveness
        type: u8
      - id: max_freed
        type: u8
      - id: obsolete1
        type: u8
      - id: obsolete2
        type: u8
      - id: obsolete3
        type: u8

  # --- Options ---

  set_options_request:
    seq:
      - id: keep_failed
        type: u8
      - id: keep_going
        type: u8
      - id: try_fallback
        type: u8
      - id: verbosity
        type: u8
      - id: max_build_jobs
        type: u8
      - id: max_silent_time
        type: u8
      - id: obsolete_use_build_hook
        type: u8
      - id: verbose_build
        type: u8
      - id: obsolete_log_type
        type: u8
      - id: obsolete_print_build_trace
        type: u8
      - id: build_cores
        type: u8
      - id: use_substitutes
        type: u8
      - id: overrides
        type: setting_overrides
        if: _root.protocol_version >= 12

  setting_overrides:
    seq:
      - id: num_settings
        type: u8
      - id: settings
        type: setting_pair
        repeat: expr
        repeat-expr: num_settings

  setting_pair:
    seq:
      - id: name
        type: nix_string
      - id: value
        type: nix_string

  # --- Verification ---

  verify_store_request:
    seq:
      - id: check_contents
        type: u8
      - id: repair
        type: u8

  # --- NAR ---

  nar_from_path_request:
    seq:
      - id: path
        type: store_path

  # --- Content-addressed outputs ---

  register_drv_output_request:
    seq:
      - id: output_id
        type: drv_output
        if: _root.protocol_version < 31
      - id: output_path
        type: nix_string
        if: _root.protocol_version < 31
      - id: realisation
        type: realisation
        if: _root.protocol_version >= 31

  query_realisation_request:
    seq:
      - id: output_id
        type: drv_output

  # --- Logs ---

  add_build_log_request:
    seq:
      - id: drv_path
        type: nix_string
        doc: StorePath as string

  # --- Derived paths ---

  derived_path_list:
    seq:
      - id: num_paths
        type: u8
      - id: paths
        type: derived_path
        repeat: expr
        repeat-expr: num_paths

  derived_path:
    doc: |
      String representation of a derived path.
      Format: "/nix/store/...-foo" or "/nix/store/...-foo.drv^out,dev"
    seq:
      - id: path
        type: nix_string

  drv_output:
    doc: Derivation output identifier (drv path + output name)
    seq:
      - id: drv_path
        type: nix_string
      - id: output_name
        type: nix_string

  # ==========================================================================
  # Response Types
  # ==========================================================================

  bool_response:
    seq:
      - id: value
        type: u8

  is_valid_path_response:
    seq:
      - id: valid
        type: u8

  query_path_info_response:
    seq:
      - id: valid
        type: u8
      - id: info
        type: unkeyed_valid_path_info
        if: valid != 0

  valid_path_info:
    seq:
      - id: path
        type: store_path
      - id: info
        type: unkeyed_valid_path_info

  unkeyed_valid_path_info:
    seq:
      - id: deriver
        type: optional_store_path
      - id: nar_hash
        type: nix_string
        doc: SHA256 hash in base16
      - id: references
        type: store_path_set
      - id: registration_time
        type: u8
      - id: nar_size
        type: u8
      - id: ultimate
        type: u8
        if: _root.protocol_version >= 16
      - id: sigs
        type: nix_string_set
        if: _root.protocol_version >= 16
      - id: ca
        type: nix_string
        if: _root.protocol_version >= 16
        doc: Content address (empty = none)

  query_substitutable_path_info_response:
    seq:
      - id: found
        type: u8
      - id: deriver
        type: optional_store_path
        if: found != 0
      - id: references
        type: store_path_set
        if: found != 0
      - id: download_size
        type: u8
        if: found != 0
      - id: nar_size
        type: u8
        if: found != 0

  query_substitutable_path_infos_response:
    seq:
      - id: num_infos
        type: u8
      - id: infos
        type: substitutable_path_info_entry
        repeat: expr
        repeat-expr: num_infos

  substitutable_path_info_entry:
    seq:
      - id: path
        type: store_path
      - id: deriver
        type: optional_store_path
      - id: references
        type: store_path_set
      - id: download_size
        type: u8
      - id: nar_size
        type: u8

  query_missing_response:
    seq:
      - id: will_build
        type: store_path_set
      - id: will_substitute
        type: store_path_set
      - id: unknown
        type: store_path_set
      - id: download_size
        type: u8
      - id: nar_size
        type: u8

  find_roots_response:
    seq:
      - id: num_roots
        type: u8
      - id: roots
        type: root_entry
        repeat: expr
        repeat-expr: num_roots

  root_entry:
    seq:
      - id: link
        type: nix_string
      - id: target
        type: store_path

  collect_garbage_response:
    seq:
      - id: paths
        type: nix_string_set
      - id: bytes_freed
        type: u8
      - id: obsolete
        type: u8

  derivation_output_map:
    seq:
      - id: num_outputs
        type: u8
      - id: outputs
        type: derivation_output_entry
        repeat: expr
        repeat-expr: num_outputs

  derivation_output_entry:
    seq:
      - id: name
        type: nix_string
      - id: path
        type: optional_store_path

  build_result:
    seq:
      - id: status
        type: u8
        enum: build_status
      - id: error_msg
        type: nix_string
      - id: times_built
        type: u8
        if: _root.protocol_version >= 29
      - id: is_non_deterministic
        type: u8
        if: _root.protocol_version >= 29
      - id: start_time
        type: u8
        if: _root.protocol_version >= 29
      - id: stop_time
        type: u8
        if: _root.protocol_version >= 29
      - id: cpu_user
        type: optional_duration
        if: _root.protocol_version >= 37
      - id: cpu_system
        type: optional_duration
        if: _root.protocol_version >= 37
      - id: built_outputs
        type: drv_outputs
        if: _root.protocol_version >= 28

  optional_duration:
    doc: Optional microseconds duration
    seq:
      - id: tag
        type: u1
      - id: microseconds
        type: s8
        if: tag == 1

  drv_outputs:
    seq:
      - id: num_outputs
        type: u8
      - id: outputs
        type: drv_output_entry
        repeat: expr
        repeat-expr: num_outputs

  drv_output_entry:
    seq:
      - id: output_id
        type: drv_output
      - id: realisation
        type: realisation

  realisation:
    seq:
      - id: id
        type: drv_output
      - id: out_path
        type: store_path
      - id: signatures
        type: nix_string_set
      - id: dependent_realisations
        type: drv_outputs

  keyed_build_result:
    seq:
      - id: path
        type: derived_path
      - id: result
        type: build_result

  build_paths_with_results_response:
    seq:
      - id: num_results
        type: u8
      - id: results
        type: keyed_build_result
        repeat: expr
        repeat-expr: num_results

  query_realisation_response_old:
    doc: Protocol < 1.31
    seq:
      - id: out_paths
        type: store_path_set

  query_realisation_response:
    doc: Protocol >= 1.31
    seq:
      - id: num_realisations
        type: u8
      - id: realisations
        type: realisation
        repeat: expr
        repeat-expr: num_realisations

  # ==========================================================================
  # Stderr Protocol (interleaved with responses)
  # ==========================================================================
  
  stderr_message:
    doc: |
      During request processing, daemon sends stderr messages.
      Client must handle these until receiving STDERR_LAST.
    seq:
      - id: msg_type
        type: u8
      - id: payload
        type:
          switch-on: msg_type
          cases:
            0x6f6c6d67: stderr_next        # STDERR_NEXT
            0x64617461: stderr_read        # STDERR_READ  
            0x64617416: stderr_write       # STDERR_WRITE
            0x616c7473: stderr_last        # STDERR_LAST
            0x63787470: stderr_error       # STDERR_ERROR
            0x53545254: stderr_start_activity
            0x53544f50: stderr_stop_activity
            0x52534c54: stderr_result

  stderr_next:
    doc: Log message from daemon
    seq:
      - id: msg
        type: nix_string

  stderr_read:
    doc: Daemon requesting data from client (for streaming uploads)
    seq:
      - id: len
        type: u8

  stderr_write:
    doc: Daemon sending data to client (for streaming downloads)  
    seq:
      - id: data
        type: nix_string

  stderr_last:
    doc: End of request processing (success)
    seq: []

  stderr_error:
    doc: Error during request processing
    seq:
      - id: error
        type: nix_error
        if: _root.protocol_version >= 26
      - id: error_msg
        type: nix_string
        if: _root.protocol_version < 26
      - id: status
        type: u8
        if: _root.protocol_version < 26

  nix_error:
    doc: Structured error (protocol >= 1.26)
    seq:
      - id: type
        type: nix_string
      - id: level
        type: u8
      - id: name
        type: nix_string
      - id: msg
        type: nix_string
      - id: have_pos
        type: u8
      - id: traces
        type: error_trace_list

  error_trace_list:
    seq:
      - id: num_traces
        type: u8
      - id: traces
        type: error_trace
        repeat: expr
        repeat-expr: num_traces

  error_trace:
    seq:
      - id: have_pos
        type: u8
      - id: msg
        type: nix_string

  stderr_start_activity:
    seq:
      - id: act_id
        type: u8
      - id: verbosity
        type: u8
      - id: activity_type
        type: u8
      - id: msg
        type: nix_string
      - id: fields
        type: logger_fields
      - id: parent
        type: u8

  stderr_stop_activity:
    seq:
      - id: act_id
        type: u8

  stderr_result:
    seq:
      - id: act_id
        type: u8
      - id: result_type
        type: u8
      - id: fields
        type: logger_fields

  logger_fields:
    seq:
      - id: num_fields
        type: u8
      - id: fields
        type: logger_field
        repeat: expr
        repeat-expr: num_fields

  logger_field:
    seq:
      - id: field_type
        type: u8
        enum: logger_field_type
      - id: int_value
        type: u8
        if: field_type == logger_field_type::int
      - id: string_value
        type: nix_string
        if: field_type == logger_field_type::string

# ==========================================================================
# Enums
# ==========================================================================

enums:
  operation:
    1: is_valid_path
    3: has_substitutes
    4: query_path_hash          # obsolete
    5: query_references         # obsolete
    6: query_referrers
    7: add_to_store
    8: add_text_to_store        # obsolete since 1.25
    9: build_paths
    10: ensure_path
    11: add_temp_root
    12: add_indirect_root
    13: sync_with_gc
    14: find_roots
    18: query_deriver           # obsolete
    19: set_options
    20: collect_garbage
    21: query_substitutable_path_info
    22: query_derivation_outputs  # obsolete
    23: query_all_valid_paths
    24: query_failed_paths        # removed
    25: clear_failed_paths        # removed
    26: query_path_info
    28: query_derivation_output_names  # obsolete
    29: query_path_from_hash_part
    30: query_substitutable_path_infos
    31: query_valid_paths
    32: query_substitutable_paths
    33: query_valid_derivers
    34: optimise_store
    35: verify_store
    36: build_derivation
    37: add_signatures
    38: nar_from_path
    39: add_to_store_nar
    40: query_missing
    41: query_derivation_output_map
    42: register_drv_output
    43: query_realisation
    44: add_multiple_to_store
    45: add_build_log
    46: build_paths_with_results
    47: add_perm_root
    48: query_active_builds

  build_mode:
    0: normal
    1: repair
    2: check

  gc_action:
    0: return_live
    1: return_dead
    2: delete_dead
    3: delete_specific

  trust_level:
    0: unknown
    1: trusted
    2: not_trusted

  build_status:
    0: built
    1: substituted
    2: already_valid
    3: permanent_failure
    4: input_rejected
    5: output_rejected
    6: transient_failure
    7: cached_failure
    8: timed_out
    9: misc_failure
    10: dependency_failed
    11: log_limit_exceeded
    12: not_deterministic
    13: resolved_drv_failed
    14: no_substituters

  logger_field_type:
    0: int
    1: string
