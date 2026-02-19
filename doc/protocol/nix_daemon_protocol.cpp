// This is a generated file! Please edit source .ksy file and use kaitai-struct-compiler to rebuild

#include "nix_daemon_protocol.h"

#include "kaitai/exceptions.h"

nix_daemon_protocol_t::nix_daemon_protocol_t(uint16_t p_protocol_version, kaitai::kstream* p__io,
                                             kaitai::kstruct* p__parent,
                                             nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = this;
  m_protocol_version = p_protocol_version;
  _read();
}

void nix_daemon_protocol_t::_read() {}

nix_daemon_protocol_t::~nix_daemon_protocol_t() {
  _clean_up();
}

void nix_daemon_protocol_t::_clean_up() {}

nix_daemon_protocol_t::valid_path_info_t::valid_path_info_t(kaitai::kstream* p__io,
                                                            kaitai::kstruct* p__parent,
                                                            nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  m_path = nullptr;
  m_info = nullptr;
  _read();
}

void nix_daemon_protocol_t::valid_path_info_t::_read() {
  m_path = std::unique_ptr<store_path_t>(new store_path_t(m__io, this, m__root));
  m_info = std::unique_ptr<unkeyed_valid_path_info_t>(
      new unkeyed_valid_path_info_t(m__io, this, m__root));
}

nix_daemon_protocol_t::valid_path_info_t::~valid_path_info_t() {
  _clean_up();
}

void nix_daemon_protocol_t::valid_path_info_t::_clean_up() {}

nix_daemon_protocol_t::stderr_stop_activity_t::stderr_stop_activity_t(
    kaitai::kstream* p__io, kaitai::kstruct* p__parent, nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  _read();
}

void nix_daemon_protocol_t::stderr_stop_activity_t::_read() {
  m_act_id = m__io->read_u8le();
}

nix_daemon_protocol_t::stderr_stop_activity_t::~stderr_stop_activity_t() {
  _clean_up();
}

void nix_daemon_protocol_t::stderr_stop_activity_t::_clean_up() {}

nix_daemon_protocol_t::add_to_store_request_t::add_to_store_request_t(
    kaitai::kstream* p__io, kaitai::kstruct* p__parent, nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  m_name = nullptr;
  m_ca_method = nullptr;
  m_refs = nullptr;
  m_base_name = nullptr;
  m_hash_algo = nullptr;
  _read();
}

void nix_daemon_protocol_t::add_to_store_request_t::_read() {
  n_name = true;
  if (_root()->protocol_version() >= 25) {
    n_name = false;
    m_name = std::unique_ptr<nix_string_t>(new nix_string_t(m__io, this, m__root));
  }
  n_ca_method = true;
  if (_root()->protocol_version() >= 25) {
    n_ca_method = false;
    m_ca_method = std::unique_ptr<nix_string_t>(new nix_string_t(m__io, this, m__root));
  }
  n_refs = true;
  if (_root()->protocol_version() >= 25) {
    n_refs = false;
    m_refs = std::unique_ptr<store_path_set_t>(new store_path_set_t(m__io, this, m__root));
  }
  n_repair = true;
  if (_root()->protocol_version() >= 25) {
    n_repair = false;
    m_repair = m__io->read_u8le();
  }
  n_base_name = true;
  if (_root()->protocol_version() < 25) {
    n_base_name = false;
    m_base_name = std::unique_ptr<nix_string_t>(new nix_string_t(m__io, this, m__root));
  }
  n_fixed = true;
  if (_root()->protocol_version() < 25) {
    n_fixed = false;
    m_fixed = m__io->read_u8le();
  }
  n_recursive = true;
  if (_root()->protocol_version() < 25) {
    n_recursive = false;
    m_recursive = m__io->read_u1();
  }
  n_hash_algo = true;
  if (_root()->protocol_version() < 25) {
    n_hash_algo = false;
    m_hash_algo = std::unique_ptr<nix_string_t>(new nix_string_t(m__io, this, m__root));
  }
}

nix_daemon_protocol_t::add_to_store_request_t::~add_to_store_request_t() {
  _clean_up();
}

void nix_daemon_protocol_t::add_to_store_request_t::_clean_up() {
  if (!n_name) {
  }
  if (!n_ca_method) {
  }
  if (!n_refs) {
  }
  if (!n_repair) {
  }
  if (!n_base_name) {
  }
  if (!n_fixed) {
  }
  if (!n_recursive) {
  }
  if (!n_hash_algo) {
  }
}

nix_daemon_protocol_t::derivation_output_entry_t::derivation_output_entry_t(
    kaitai::kstream* p__io, kaitai::kstruct* p__parent, nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  m_name = nullptr;
  m_path = nullptr;
  _read();
}

void nix_daemon_protocol_t::derivation_output_entry_t::_read() {
  m_name = std::unique_ptr<nix_string_t>(new nix_string_t(m__io, this, m__root));
  m_path = std::unique_ptr<optional_store_path_t>(new optional_store_path_t(m__io, this, m__root));
}

nix_daemon_protocol_t::derivation_output_entry_t::~derivation_output_entry_t() {
  _clean_up();
}

void nix_daemon_protocol_t::derivation_output_entry_t::_clean_up() {}

nix_daemon_protocol_t::query_valid_paths_request_t::query_valid_paths_request_t(
    kaitai::kstream* p__io, kaitai::kstruct* p__parent, nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  m_paths = nullptr;
  _read();
}

void nix_daemon_protocol_t::query_valid_paths_request_t::_read() {
  m_paths = std::unique_ptr<store_path_set_t>(new store_path_set_t(m__io, this, m__root));
  n_substitute = true;
  if (_root()->protocol_version() >= 27) {
    n_substitute = false;
    m_substitute = m__io->read_u8le();
  }
}

nix_daemon_protocol_t::query_valid_paths_request_t::~query_valid_paths_request_t() {
  _clean_up();
}

void nix_daemon_protocol_t::query_valid_paths_request_t::_clean_up() {
  if (!n_substitute) {
  }
}

nix_daemon_protocol_t::realisation_t::realisation_t(kaitai::kstream* p__io,
                                                    kaitai::kstruct* p__parent,
                                                    nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  m_id = nullptr;
  m_out_path = nullptr;
  m_signatures = nullptr;
  m_dependent_realisations = nullptr;
  _read();
}

void nix_daemon_protocol_t::realisation_t::_read() {
  m_id = std::unique_ptr<drv_output_t>(new drv_output_t(m__io, this, m__root));
  m_out_path = std::unique_ptr<store_path_t>(new store_path_t(m__io, this, m__root));
  m_signatures = std::unique_ptr<nix_string_set_t>(new nix_string_set_t(m__io, this, m__root));
  m_dependent_realisations =
      std::unique_ptr<drv_outputs_t>(new drv_outputs_t(m__io, this, m__root));
}

nix_daemon_protocol_t::realisation_t::~realisation_t() {
  _clean_up();
}

void nix_daemon_protocol_t::realisation_t::_clean_up() {}

nix_daemon_protocol_t::query_missing_response_t::query_missing_response_t(
    kaitai::kstream* p__io, kaitai::kstruct* p__parent, nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  m_will_build = nullptr;
  m_will_substitute = nullptr;
  m_unknown = nullptr;
  _read();
}

void nix_daemon_protocol_t::query_missing_response_t::_read() {
  m_will_build = std::unique_ptr<store_path_set_t>(new store_path_set_t(m__io, this, m__root));
  m_will_substitute = std::unique_ptr<store_path_set_t>(new store_path_set_t(m__io, this, m__root));
  m_unknown = std::unique_ptr<store_path_set_t>(new store_path_set_t(m__io, this, m__root));
  m_download_size = m__io->read_u8le();
  m_nar_size = m__io->read_u8le();
}

nix_daemon_protocol_t::query_missing_response_t::~query_missing_response_t() {
  _clean_up();
}

void nix_daemon_protocol_t::query_missing_response_t::_clean_up() {}

nix_daemon_protocol_t::request_t::request_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent,
                                            nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  _read();
}

void nix_daemon_protocol_t::request_t::_read() {
  m_op = static_cast<nix_daemon_protocol_t::operation_t>(m__io->read_u8le());
  n_payload = true;
  switch (op()) {
    case nix_daemon_protocol_t::OPERATION_QUERY_DERIVATION_OUTPUT_MAP: {
      n_payload = false;
      m_payload = std::unique_ptr<query_derivation_output_map_request_t>(
          new query_derivation_output_map_request_t(m__io, this, m__root));
      break;
    }
    case nix_daemon_protocol_t::OPERATION_NAR_FROM_PATH: {
      n_payload = false;
      m_payload = std::unique_ptr<nar_from_path_request_t>(
          new nar_from_path_request_t(m__io, this, m__root));
      break;
    }
    case nix_daemon_protocol_t::OPERATION_QUERY_PATH_FROM_HASH_PART: {
      n_payload = false;
      m_payload = std::unique_ptr<query_path_from_hash_part_request_t>(
          new query_path_from_hash_part_request_t(m__io, this, m__root));
      break;
    }
    case nix_daemon_protocol_t::OPERATION_FIND_ROOTS: {
      n_payload = false;
      m_payload = std::unique_ptr<empty_request_t>(new empty_request_t(m__io, this, m__root));
      break;
    }
    case nix_daemon_protocol_t::OPERATION_QUERY_REFERRERS: {
      n_payload = false;
      m_payload = std::unique_ptr<query_referrers_request_t>(
          new query_referrers_request_t(m__io, this, m__root));
      break;
    }
    case nix_daemon_protocol_t::OPERATION_ADD_TO_STORE_NAR: {
      n_payload = false;
      m_payload = std::unique_ptr<add_to_store_nar_request_t>(
          new add_to_store_nar_request_t(m__io, this, m__root));
      break;
    }
    case nix_daemon_protocol_t::OPERATION_QUERY_VALID_DERIVERS: {
      n_payload = false;
      m_payload = std::unique_ptr<query_valid_derivers_request_t>(
          new query_valid_derivers_request_t(m__io, this, m__root));
      break;
    }
    case nix_daemon_protocol_t::OPERATION_BUILD_PATHS: {
      n_payload = false;
      m_payload =
          std::unique_ptr<build_paths_request_t>(new build_paths_request_t(m__io, this, m__root));
      break;
    }
    case nix_daemon_protocol_t::OPERATION_QUERY_DERIVATION_OUTPUT_NAMES: {
      n_payload = false;
      m_payload = std::unique_ptr<query_derivation_output_names_request_t>(
          new query_derivation_output_names_request_t(m__io, this, m__root));
      break;
    }
    case nix_daemon_protocol_t::OPERATION_ENSURE_PATH: {
      n_payload = false;
      m_payload =
          std::unique_ptr<ensure_path_request_t>(new ensure_path_request_t(m__io, this, m__root));
      break;
    }
    case nix_daemon_protocol_t::OPERATION_ADD_SIGNATURES: {
      n_payload = false;
      m_payload = std::unique_ptr<add_signatures_request_t>(
          new add_signatures_request_t(m__io, this, m__root));
      break;
    }
    case nix_daemon_protocol_t::OPERATION_QUERY_SUBSTITUTABLE_PATH_INFOS: {
      n_payload = false;
      m_payload = std::unique_ptr<query_substitutable_path_infos_request_t>(
          new query_substitutable_path_infos_request_t(m__io, this, m__root));
      break;
    }
    case nix_daemon_protocol_t::OPERATION_ADD_TEXT_TO_STORE: {
      n_payload = false;
      m_payload = std::unique_ptr<add_text_to_store_request_t>(
          new add_text_to_store_request_t(m__io, this, m__root));
      break;
    }
    case nix_daemon_protocol_t::OPERATION_QUERY_SUBSTITUTABLE_PATH_INFO: {
      n_payload = false;
      m_payload = std::unique_ptr<query_substitutable_path_info_request_t>(
          new query_substitutable_path_info_request_t(m__io, this, m__root));
      break;
    }
    case nix_daemon_protocol_t::OPERATION_QUERY_MISSING: {
      n_payload = false;
      m_payload = std::unique_ptr<query_missing_request_t>(
          new query_missing_request_t(m__io, this, m__root));
      break;
    }
    case nix_daemon_protocol_t::OPERATION_IS_VALID_PATH: {
      n_payload = false;
      m_payload = std::unique_ptr<is_valid_path_request_t>(
          new is_valid_path_request_t(m__io, this, m__root));
      break;
    }
    case nix_daemon_protocol_t::OPERATION_QUERY_PATH_HASH: {
      n_payload = false;
      m_payload = std::unique_ptr<query_path_hash_request_t>(
          new query_path_hash_request_t(m__io, this, m__root));
      break;
    }
    case nix_daemon_protocol_t::OPERATION_SYNC_WITH_GC: {
      n_payload = false;
      m_payload = std::unique_ptr<empty_request_t>(new empty_request_t(m__io, this, m__root));
      break;
    }
    case nix_daemon_protocol_t::OPERATION_ADD_PERM_ROOT: {
      n_payload = false;
      m_payload = std::unique_ptr<add_perm_root_request_t>(
          new add_perm_root_request_t(m__io, this, m__root));
      break;
    }
    case nix_daemon_protocol_t::OPERATION_ADD_MULTIPLE_TO_STORE: {
      n_payload = false;
      m_payload = std::unique_ptr<add_multiple_to_store_request_t>(
          new add_multiple_to_store_request_t(m__io, this, m__root));
      break;
    }
    case nix_daemon_protocol_t::OPERATION_HAS_SUBSTITUTES: {
      n_payload = false;
      m_payload = std::unique_ptr<has_substitutes_request_t>(
          new has_substitutes_request_t(m__io, this, m__root));
      break;
    }
    case nix_daemon_protocol_t::OPERATION_OPTIMISE_STORE: {
      n_payload = false;
      m_payload = std::unique_ptr<empty_request_t>(new empty_request_t(m__io, this, m__root));
      break;
    }
    case nix_daemon_protocol_t::OPERATION_ADD_INDIRECT_ROOT: {
      n_payload = false;
      m_payload = std::unique_ptr<add_indirect_root_request_t>(
          new add_indirect_root_request_t(m__io, this, m__root));
      break;
    }
    case nix_daemon_protocol_t::OPERATION_QUERY_PATH_INFO: {
      n_payload = false;
      m_payload = std::unique_ptr<query_path_info_request_t>(
          new query_path_info_request_t(m__io, this, m__root));
      break;
    }
    case nix_daemon_protocol_t::OPERATION_QUERY_ACTIVE_BUILDS: {
      n_payload = false;
      m_payload = std::unique_ptr<empty_request_t>(new empty_request_t(m__io, this, m__root));
      break;
    }
    case nix_daemon_protocol_t::OPERATION_QUERY_SUBSTITUTABLE_PATHS: {
      n_payload = false;
      m_payload = std::unique_ptr<query_substitutable_paths_request_t>(
          new query_substitutable_paths_request_t(m__io, this, m__root));
      break;
    }
    case nix_daemon_protocol_t::OPERATION_REGISTER_DRV_OUTPUT: {
      n_payload = false;
      m_payload = std::unique_ptr<register_drv_output_request_t>(
          new register_drv_output_request_t(m__io, this, m__root));
      break;
    }
    case nix_daemon_protocol_t::OPERATION_ADD_TEMP_ROOT: {
      n_payload = false;
      m_payload = std::unique_ptr<add_temp_root_request_t>(
          new add_temp_root_request_t(m__io, this, m__root));
      break;
    }
    case nix_daemon_protocol_t::OPERATION_BUILD_PATHS_WITH_RESULTS: {
      n_payload = false;
      m_payload = std::unique_ptr<build_paths_with_results_request_t>(
          new build_paths_with_results_request_t(m__io, this, m__root));
      break;
    }
    case nix_daemon_protocol_t::OPERATION_QUERY_REALISATION: {
      n_payload = false;
      m_payload = std::unique_ptr<query_realisation_request_t>(
          new query_realisation_request_t(m__io, this, m__root));
      break;
    }
    case nix_daemon_protocol_t::OPERATION_BUILD_DERIVATION: {
      n_payload = false;
      m_payload = std::unique_ptr<build_derivation_request_t>(
          new build_derivation_request_t(m__io, this, m__root));
      break;
    }
    case nix_daemon_protocol_t::OPERATION_QUERY_DERIVATION_OUTPUTS: {
      n_payload = false;
      m_payload = std::unique_ptr<query_derivation_outputs_request_t>(
          new query_derivation_outputs_request_t(m__io, this, m__root));
      break;
    }
    case nix_daemon_protocol_t::OPERATION_COLLECT_GARBAGE: {
      n_payload = false;
      m_payload = std::unique_ptr<collect_garbage_request_t>(
          new collect_garbage_request_t(m__io, this, m__root));
      break;
    }
    case nix_daemon_protocol_t::OPERATION_QUERY_REFERENCES: {
      n_payload = false;
      m_payload = std::unique_ptr<query_references_request_t>(
          new query_references_request_t(m__io, this, m__root));
      break;
    }
    case nix_daemon_protocol_t::OPERATION_VERIFY_STORE: {
      n_payload = false;
      m_payload =
          std::unique_ptr<verify_store_request_t>(new verify_store_request_t(m__io, this, m__root));
      break;
    }
    case nix_daemon_protocol_t::OPERATION_ADD_TO_STORE: {
      n_payload = false;
      m_payload =
          std::unique_ptr<add_to_store_request_t>(new add_to_store_request_t(m__io, this, m__root));
      break;
    }
    case nix_daemon_protocol_t::OPERATION_QUERY_DERIVER: {
      n_payload = false;
      m_payload = std::unique_ptr<query_deriver_request_t>(
          new query_deriver_request_t(m__io, this, m__root));
      break;
    }
    case nix_daemon_protocol_t::OPERATION_ADD_BUILD_LOG: {
      n_payload = false;
      m_payload = std::unique_ptr<add_build_log_request_t>(
          new add_build_log_request_t(m__io, this, m__root));
      break;
    }
    case nix_daemon_protocol_t::OPERATION_SET_OPTIONS: {
      n_payload = false;
      m_payload =
          std::unique_ptr<set_options_request_t>(new set_options_request_t(m__io, this, m__root));
      break;
    }
    case nix_daemon_protocol_t::OPERATION_QUERY_VALID_PATHS: {
      n_payload = false;
      m_payload = std::unique_ptr<query_valid_paths_request_t>(
          new query_valid_paths_request_t(m__io, this, m__root));
      break;
    }
    case nix_daemon_protocol_t::OPERATION_QUERY_ALL_VALID_PATHS: {
      n_payload = false;
      m_payload = std::unique_ptr<empty_request_t>(new empty_request_t(m__io, this, m__root));
      break;
    }
  }
}

nix_daemon_protocol_t::request_t::~request_t() {
  _clean_up();
}

void nix_daemon_protocol_t::request_t::_clean_up() {
  if (!n_payload) {
  }
}

nix_daemon_protocol_t::drv_output_t::drv_output_t(kaitai::kstream* p__io,
                                                  kaitai::kstruct* p__parent,
                                                  nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  m_drv_path = nullptr;
  m_output_name = nullptr;
  _read();
}

void nix_daemon_protocol_t::drv_output_t::_read() {
  m_drv_path = std::unique_ptr<nix_string_t>(new nix_string_t(m__io, this, m__root));
  m_output_name = std::unique_ptr<nix_string_t>(new nix_string_t(m__io, this, m__root));
}

nix_daemon_protocol_t::drv_output_t::~drv_output_t() {
  _clean_up();
}

void nix_daemon_protocol_t::drv_output_t::_clean_up() {}

nix_daemon_protocol_t::add_build_log_request_t::add_build_log_request_t(
    kaitai::kstream* p__io, kaitai::kstruct* p__parent, nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  m_drv_path = nullptr;
  _read();
}

void nix_daemon_protocol_t::add_build_log_request_t::_read() {
  m_drv_path = std::unique_ptr<nix_string_t>(new nix_string_t(m__io, this, m__root));
}

nix_daemon_protocol_t::add_build_log_request_t::~add_build_log_request_t() {
  _clean_up();
}

void nix_daemon_protocol_t::add_build_log_request_t::_clean_up() {}

nix_daemon_protocol_t::add_text_to_store_request_t::add_text_to_store_request_t(
    kaitai::kstream* p__io, kaitai::kstruct* p__parent, nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  m_suffix = nullptr;
  m_text = nullptr;
  m_refs = nullptr;
  _read();
}

void nix_daemon_protocol_t::add_text_to_store_request_t::_read() {
  m_suffix = std::unique_ptr<nix_string_t>(new nix_string_t(m__io, this, m__root));
  m_text = std::unique_ptr<nix_string_t>(new nix_string_t(m__io, this, m__root));
  m_refs = std::unique_ptr<store_path_set_t>(new store_path_set_t(m__io, this, m__root));
}

nix_daemon_protocol_t::add_text_to_store_request_t::~add_text_to_store_request_t() {
  _clean_up();
}

void nix_daemon_protocol_t::add_text_to_store_request_t::_clean_up() {}

nix_daemon_protocol_t::find_roots_response_t::find_roots_response_t(kaitai::kstream* p__io,
                                                                    kaitai::kstruct* p__parent,
                                                                    nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  m_roots = nullptr;
  _read();
}

void nix_daemon_protocol_t::find_roots_response_t::_read() {
  m_num_roots = m__io->read_u8le();
  m_roots = std::unique_ptr<std::vector<std::unique_ptr<root_entry_t>>>(
      new std::vector<std::unique_ptr<root_entry_t>>());
  const int l_roots = num_roots();
  for (int i = 0; i < l_roots; i++) {
    m_roots->push_back(
        std::move(std::unique_ptr<root_entry_t>(new root_entry_t(m__io, this, m__root))));
  }
}

nix_daemon_protocol_t::find_roots_response_t::~find_roots_response_t() {
  _clean_up();
}

void nix_daemon_protocol_t::find_roots_response_t::_clean_up() {}

nix_daemon_protocol_t::collect_garbage_response_t::collect_garbage_response_t(
    kaitai::kstream* p__io, kaitai::kstruct* p__parent, nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  m_paths = nullptr;
  _read();
}

void nix_daemon_protocol_t::collect_garbage_response_t::_read() {
  m_paths = std::unique_ptr<nix_string_set_t>(new nix_string_set_t(m__io, this, m__root));
  m_bytes_freed = m__io->read_u8le();
  m_obsolete = m__io->read_u8le();
}

nix_daemon_protocol_t::collect_garbage_response_t::~collect_garbage_response_t() {
  _clean_up();
}

void nix_daemon_protocol_t::collect_garbage_response_t::_clean_up() {}

nix_daemon_protocol_t::add_temp_root_request_t::add_temp_root_request_t(
    kaitai::kstream* p__io, kaitai::kstruct* p__parent, nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  m_path = nullptr;
  _read();
}

void nix_daemon_protocol_t::add_temp_root_request_t::_read() {
  m_path = std::unique_ptr<store_path_t>(new store_path_t(m__io, this, m__root));
}

nix_daemon_protocol_t::add_temp_root_request_t::~add_temp_root_request_t() {
  _clean_up();
}

void nix_daemon_protocol_t::add_temp_root_request_t::_clean_up() {}

nix_daemon_protocol_t::setting_overrides_t::setting_overrides_t(kaitai::kstream* p__io,
                                                                kaitai::kstruct* p__parent,
                                                                nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  m_settings = nullptr;
  _read();
}

void nix_daemon_protocol_t::setting_overrides_t::_read() {
  m_num_settings = m__io->read_u8le();
  m_settings = std::unique_ptr<std::vector<std::unique_ptr<setting_pair_t>>>(
      new std::vector<std::unique_ptr<setting_pair_t>>());
  const int l_settings = num_settings();
  for (int i = 0; i < l_settings; i++) {
    m_settings->push_back(
        std::move(std::unique_ptr<setting_pair_t>(new setting_pair_t(m__io, this, m__root))));
  }
}

nix_daemon_protocol_t::setting_overrides_t::~setting_overrides_t() {
  _clean_up();
}

void nix_daemon_protocol_t::setting_overrides_t::_clean_up() {}

nix_daemon_protocol_t::query_substitutable_path_infos_response_t::
    query_substitutable_path_infos_response_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent,
                                              nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  m_infos = nullptr;
  _read();
}

void nix_daemon_protocol_t::query_substitutable_path_infos_response_t::_read() {
  m_num_infos = m__io->read_u8le();
  m_infos = std::unique_ptr<std::vector<std::unique_ptr<substitutable_path_info_entry_t>>>(
      new std::vector<std::unique_ptr<substitutable_path_info_entry_t>>());
  const int l_infos = num_infos();
  for (int i = 0; i < l_infos; i++) {
    m_infos->push_back(std::move(std::unique_ptr<substitutable_path_info_entry_t>(
        new substitutable_path_info_entry_t(m__io, this, m__root))));
  }
}

nix_daemon_protocol_t::query_substitutable_path_infos_response_t::
    ~query_substitutable_path_infos_response_t() {
  _clean_up();
}

void nix_daemon_protocol_t::query_substitutable_path_infos_response_t::_clean_up() {}

nix_daemon_protocol_t::stderr_result_t::stderr_result_t(kaitai::kstream* p__io,
                                                        kaitai::kstruct* p__parent,
                                                        nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  m_fields = nullptr;
  _read();
}

void nix_daemon_protocol_t::stderr_result_t::_read() {
  m_act_id = m__io->read_u8le();
  m_result_type = m__io->read_u8le();
  m_fields = std::unique_ptr<logger_fields_t>(new logger_fields_t(m__io, this, m__root));
}

nix_daemon_protocol_t::stderr_result_t::~stderr_result_t() {
  _clean_up();
}

void nix_daemon_protocol_t::stderr_result_t::_clean_up() {}

nix_daemon_protocol_t::setting_pair_t::setting_pair_t(kaitai::kstream* p__io,
                                                      kaitai::kstruct* p__parent,
                                                      nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  m_name = nullptr;
  m_value = nullptr;
  _read();
}

void nix_daemon_protocol_t::setting_pair_t::_read() {
  m_name = std::unique_ptr<nix_string_t>(new nix_string_t(m__io, this, m__root));
  m_value = std::unique_ptr<nix_string_t>(new nix_string_t(m__io, this, m__root));
}

nix_daemon_protocol_t::setting_pair_t::~setting_pair_t() {
  _clean_up();
}

void nix_daemon_protocol_t::setting_pair_t::_clean_up() {}

nix_daemon_protocol_t::is_valid_path_response_t::is_valid_path_response_t(
    kaitai::kstream* p__io, kaitai::kstruct* p__parent, nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  _read();
}

void nix_daemon_protocol_t::is_valid_path_response_t::_read() {
  m_valid = m__io->read_u8le();
}

nix_daemon_protocol_t::is_valid_path_response_t::~is_valid_path_response_t() {
  _clean_up();
}

void nix_daemon_protocol_t::is_valid_path_response_t::_clean_up() {}

nix_daemon_protocol_t::logger_fields_t::logger_fields_t(kaitai::kstream* p__io,
                                                        kaitai::kstruct* p__parent,
                                                        nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  m_fields = nullptr;
  _read();
}

void nix_daemon_protocol_t::logger_fields_t::_read() {
  m_num_fields = m__io->read_u8le();
  m_fields = std::unique_ptr<std::vector<std::unique_ptr<logger_field_t>>>(
      new std::vector<std::unique_ptr<logger_field_t>>());
  const int l_fields = num_fields();
  for (int i = 0; i < l_fields; i++) {
    m_fields->push_back(
        std::move(std::unique_ptr<logger_field_t>(new logger_field_t(m__io, this, m__root))));
  }
}

nix_daemon_protocol_t::logger_fields_t::~logger_fields_t() {
  _clean_up();
}

void nix_daemon_protocol_t::logger_fields_t::_clean_up() {}

nix_daemon_protocol_t::optional_duration_t::optional_duration_t(kaitai::kstream* p__io,
                                                                kaitai::kstruct* p__parent,
                                                                nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  _read();
}

void nix_daemon_protocol_t::optional_duration_t::_read() {
  m_tag = m__io->read_u1();
  n_microseconds = true;
  if (tag() == 1) {
    n_microseconds = false;
    m_microseconds = m__io->read_s8le();
  }
}

nix_daemon_protocol_t::optional_duration_t::~optional_duration_t() {
  _clean_up();
}

void nix_daemon_protocol_t::optional_duration_t::_clean_up() {
  if (!n_microseconds) {
  }
}

nix_daemon_protocol_t::has_substitutes_request_t::has_substitutes_request_t(
    kaitai::kstream* p__io, kaitai::kstruct* p__parent, nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  m_path = nullptr;
  _read();
}

void nix_daemon_protocol_t::has_substitutes_request_t::_read() {
  m_path = std::unique_ptr<store_path_t>(new store_path_t(m__io, this, m__root));
}

nix_daemon_protocol_t::has_substitutes_request_t::~has_substitutes_request_t() {
  _clean_up();
}

void nix_daemon_protocol_t::has_substitutes_request_t::_clean_up() {}

nix_daemon_protocol_t::nar_from_path_request_t::nar_from_path_request_t(
    kaitai::kstream* p__io, kaitai::kstruct* p__parent, nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  m_path = nullptr;
  _read();
}

void nix_daemon_protocol_t::nar_from_path_request_t::_read() {
  m_path = std::unique_ptr<store_path_t>(new store_path_t(m__io, this, m__root));
}

nix_daemon_protocol_t::nar_from_path_request_t::~nar_from_path_request_t() {
  _clean_up();
}

void nix_daemon_protocol_t::nar_from_path_request_t::_clean_up() {}

nix_daemon_protocol_t::nix_string_set_t::nix_string_set_t(kaitai::kstream* p__io,
                                                          kaitai::kstruct* p__parent,
                                                          nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  m_items = nullptr;
  _read();
}

void nix_daemon_protocol_t::nix_string_set_t::_read() {
  m_num_items = m__io->read_u8le();
  m_items = std::unique_ptr<std::vector<std::unique_ptr<nix_string_t>>>(
      new std::vector<std::unique_ptr<nix_string_t>>());
  const int l_items = num_items();
  for (int i = 0; i < l_items; i++) {
    m_items->push_back(
        std::move(std::unique_ptr<nix_string_t>(new nix_string_t(m__io, this, m__root))));
  }
}

nix_daemon_protocol_t::nix_string_set_t::~nix_string_set_t() {
  _clean_up();
}

void nix_daemon_protocol_t::nix_string_set_t::_clean_up() {}

nix_daemon_protocol_t::server_handshake_info_t::server_handshake_info_t(
    kaitai::kstream* p__io, kaitai::kstruct* p__parent, nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  m_daemon_version = nullptr;
  _read();
}

void nix_daemon_protocol_t::server_handshake_info_t::_read() {
  n_daemon_version = true;
  if (_root()->protocol_version() >= 33) {
    n_daemon_version = false;
    m_daemon_version = std::unique_ptr<nix_string_t>(new nix_string_t(m__io, this, m__root));
  }
  n_trust_level = true;
  if (_root()->protocol_version() >= 35) {
    n_trust_level = false;
    m_trust_level = static_cast<nix_daemon_protocol_t::trust_level_t>(m__io->read_u8le());
  }
}

nix_daemon_protocol_t::server_handshake_info_t::~server_handshake_info_t() {
  _clean_up();
}

void nix_daemon_protocol_t::server_handshake_info_t::_clean_up() {
  if (!n_daemon_version) {
  }
  if (!n_trust_level) {
  }
}

nix_daemon_protocol_t::add_signatures_request_t::add_signatures_request_t(
    kaitai::kstream* p__io, kaitai::kstruct* p__parent, nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  m_path = nullptr;
  m_sigs = nullptr;
  _read();
}

void nix_daemon_protocol_t::add_signatures_request_t::_read() {
  m_path = std::unique_ptr<store_path_t>(new store_path_t(m__io, this, m__root));
  m_sigs = std::unique_ptr<nix_string_set_t>(new nix_string_set_t(m__io, this, m__root));
}

nix_daemon_protocol_t::add_signatures_request_t::~add_signatures_request_t() {
  _clean_up();
}

void nix_daemon_protocol_t::add_signatures_request_t::_clean_up() {}

nix_daemon_protocol_t::nix_error_t::nix_error_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent,
                                                nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  m_type = nullptr;
  m_name = nullptr;
  m_msg = nullptr;
  m_traces = nullptr;
  _read();
}

void nix_daemon_protocol_t::nix_error_t::_read() {
  m_type = std::unique_ptr<nix_string_t>(new nix_string_t(m__io, this, m__root));
  m_level = m__io->read_u8le();
  m_name = std::unique_ptr<nix_string_t>(new nix_string_t(m__io, this, m__root));
  m_msg = std::unique_ptr<nix_string_t>(new nix_string_t(m__io, this, m__root));
  m_have_pos = m__io->read_u8le();
  m_traces = std::unique_ptr<error_trace_list_t>(new error_trace_list_t(m__io, this, m__root));
}

nix_daemon_protocol_t::nix_error_t::~nix_error_t() {
  _clean_up();
}

void nix_daemon_protocol_t::nix_error_t::_clean_up() {}

nix_daemon_protocol_t::query_references_request_t::query_references_request_t(
    kaitai::kstream* p__io, kaitai::kstruct* p__parent, nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  m_path = nullptr;
  _read();
}

void nix_daemon_protocol_t::query_references_request_t::_read() {
  m_path = std::unique_ptr<store_path_t>(new store_path_t(m__io, this, m__root));
}

nix_daemon_protocol_t::query_references_request_t::~query_references_request_t() {
  _clean_up();
}

void nix_daemon_protocol_t::query_references_request_t::_clean_up() {}

nix_daemon_protocol_t::is_valid_path_request_t::is_valid_path_request_t(
    kaitai::kstream* p__io, kaitai::kstruct* p__parent, nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  m_path = nullptr;
  _read();
}

void nix_daemon_protocol_t::is_valid_path_request_t::_read() {
  m_path = std::unique_ptr<store_path_t>(new store_path_t(m__io, this, m__root));
}

nix_daemon_protocol_t::is_valid_path_request_t::~is_valid_path_request_t() {
  _clean_up();
}

void nix_daemon_protocol_t::is_valid_path_request_t::_clean_up() {}

nix_daemon_protocol_t::store_path_t::store_path_t(kaitai::kstream* p__io,
                                                  kaitai::kstruct* p__parent,
                                                  nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  m_path = nullptr;
  _read();
}

void nix_daemon_protocol_t::store_path_t::_read() {
  m_path = std::unique_ptr<nix_string_t>(new nix_string_t(m__io, this, m__root));
}

nix_daemon_protocol_t::store_path_t::~store_path_t() {
  _clean_up();
}

void nix_daemon_protocol_t::store_path_t::_clean_up() {}

nix_daemon_protocol_t::query_substitutable_paths_request_t::query_substitutable_paths_request_t(
    kaitai::kstream* p__io, kaitai::kstruct* p__parent, nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  m_paths = nullptr;
  _read();
}

void nix_daemon_protocol_t::query_substitutable_paths_request_t::_read() {
  m_paths = std::unique_ptr<store_path_set_t>(new store_path_set_t(m__io, this, m__root));
}

nix_daemon_protocol_t::query_substitutable_paths_request_t::~query_substitutable_paths_request_t() {
  _clean_up();
}

void nix_daemon_protocol_t::query_substitutable_paths_request_t::_clean_up() {}

nix_daemon_protocol_t::client_handshake_continuation_t::client_handshake_continuation_t(
    kaitai::kstream* p__io, kaitai::kstruct* p__parent, nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  _read();
}

void nix_daemon_protocol_t::client_handshake_continuation_t::_read() {
  n_cpu_affinity_tag = true;
  if (_root()->protocol_version() >= 14) {
    n_cpu_affinity_tag = false;
    m_cpu_affinity_tag = m__io->read_u8le();
  }
  n_cpu_affinity = true;
  if (((_root()->protocol_version() >= 14) && (cpu_affinity_tag() != 0))) {
    n_cpu_affinity = false;
    m_cpu_affinity = m__io->read_u8le();
  }
  n_reserve_space = true;
  if (_root()->protocol_version() >= 11) {
    n_reserve_space = false;
    m_reserve_space = m__io->read_u8le();
  }
}

nix_daemon_protocol_t::client_handshake_continuation_t::~client_handshake_continuation_t() {
  _clean_up();
}

void nix_daemon_protocol_t::client_handshake_continuation_t::_clean_up() {
  if (!n_cpu_affinity_tag) {
  }
  if (!n_cpu_affinity) {
  }
  if (!n_reserve_space) {
  }
}

nix_daemon_protocol_t::stderr_message_t::stderr_message_t(kaitai::kstream* p__io,
                                                          kaitai::kstruct* p__parent,
                                                          nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  _read();
}

void nix_daemon_protocol_t::stderr_message_t::_read() {
  m_msg_type = m__io->read_u8le();
  n_payload = true;
  switch (msg_type()) {
    case 1634497651: {
      n_payload = false;
      m_payload = std::unique_ptr<stderr_last_t>(new stderr_last_t(m__io, this, m__root));
      break;
    }
    case 1381190740: {
      n_payload = false;
      m_payload = std::unique_ptr<stderr_result_t>(new stderr_result_t(m__io, this, m__root));
      break;
    }
    case 1684108310: {
      n_payload = false;
      m_payload = std::unique_ptr<stderr_write_t>(new stderr_write_t(m__io, this, m__root));
      break;
    }
    case 1684108385: {
      n_payload = false;
      m_payload = std::unique_ptr<stderr_read_t>(new stderr_read_t(m__io, this, m__root));
      break;
    }
    case 1668838512: {
      n_payload = false;
      m_payload = std::unique_ptr<stderr_error_t>(new stderr_error_t(m__io, this, m__root));
      break;
    }
    case 1869376871: {
      n_payload = false;
      m_payload = std::unique_ptr<stderr_next_t>(new stderr_next_t(m__io, this, m__root));
      break;
    }
    case 1398034256: {
      n_payload = false;
      m_payload =
          std::unique_ptr<stderr_stop_activity_t>(new stderr_stop_activity_t(m__io, this, m__root));
      break;
    }
    case 1398035028: {
      n_payload = false;
      m_payload = std::unique_ptr<stderr_start_activity_t>(
          new stderr_start_activity_t(m__io, this, m__root));
      break;
    }
  }
}

nix_daemon_protocol_t::stderr_message_t::~stderr_message_t() {
  _clean_up();
}

void nix_daemon_protocol_t::stderr_message_t::_clean_up() {
  if (!n_payload) {
  }
}

nix_daemon_protocol_t::stderr_start_activity_t::stderr_start_activity_t(
    kaitai::kstream* p__io, kaitai::kstruct* p__parent, nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  m_msg = nullptr;
  m_fields = nullptr;
  _read();
}

void nix_daemon_protocol_t::stderr_start_activity_t::_read() {
  m_act_id = m__io->read_u8le();
  m_verbosity = m__io->read_u8le();
  m_activity_type = m__io->read_u8le();
  m_msg = std::unique_ptr<nix_string_t>(new nix_string_t(m__io, this, m__root));
  m_fields = std::unique_ptr<logger_fields_t>(new logger_fields_t(m__io, this, m__root));
  m_parent = m__io->read_u8le();
}

nix_daemon_protocol_t::stderr_start_activity_t::~stderr_start_activity_t() {
  _clean_up();
}

void nix_daemon_protocol_t::stderr_start_activity_t::_clean_up() {}

nix_daemon_protocol_t::store_path_set_t::store_path_set_t(kaitai::kstream* p__io,
                                                          kaitai::kstruct* p__parent,
                                                          nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  m_paths = nullptr;
  _read();
}

void nix_daemon_protocol_t::store_path_set_t::_read() {
  m_num_paths = m__io->read_u8le();
  m_paths = std::unique_ptr<std::vector<std::unique_ptr<store_path_t>>>(
      new std::vector<std::unique_ptr<store_path_t>>());
  const int l_paths = num_paths();
  for (int i = 0; i < l_paths; i++) {
    m_paths->push_back(
        std::move(std::unique_ptr<store_path_t>(new store_path_t(m__io, this, m__root))));
  }
}

nix_daemon_protocol_t::store_path_set_t::~store_path_set_t() {
  _clean_up();
}

void nix_daemon_protocol_t::store_path_set_t::_clean_up() {}

nix_daemon_protocol_t::store_path_ca_entry_t::store_path_ca_entry_t(kaitai::kstream* p__io,
                                                                    kaitai::kstruct* p__parent,
                                                                    nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  m_path = nullptr;
  m_ca = nullptr;
  _read();
}

void nix_daemon_protocol_t::store_path_ca_entry_t::_read() {
  m_path = std::unique_ptr<store_path_t>(new store_path_t(m__io, this, m__root));
  m_ca = std::unique_ptr<nix_string_t>(new nix_string_t(m__io, this, m__root));
}

nix_daemon_protocol_t::store_path_ca_entry_t::~store_path_ca_entry_t() {
  _clean_up();
}

void nix_daemon_protocol_t::store_path_ca_entry_t::_clean_up() {}

nix_daemon_protocol_t::nix_string_t::nix_string_t(kaitai::kstream* p__io,
                                                  kaitai::kstruct* p__parent,
                                                  nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  _read();
}

void nix_daemon_protocol_t::nix_string_t::_read() {
  m_len = m__io->read_u8le();
  m_data = kaitai::kstream::bytes_to_str(m__io->read_bytes(len()), "UTF-8");
  m_padding = m__io->read_bytes(kaitai::kstream::mod((8 - kaitai::kstream::mod(len(), 8)), 8));
}

nix_daemon_protocol_t::nix_string_t::~nix_string_t() {
  _clean_up();
}

void nix_daemon_protocol_t::nix_string_t::_clean_up() {}

nix_daemon_protocol_t::build_paths_with_results_request_t::build_paths_with_results_request_t(
    kaitai::kstream* p__io, kaitai::kstruct* p__parent, nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  m_paths = nullptr;
  _read();
}

void nix_daemon_protocol_t::build_paths_with_results_request_t::_read() {
  m_paths = std::unique_ptr<derived_path_list_t>(new derived_path_list_t(m__io, this, m__root));
  m_build_mode = static_cast<nix_daemon_protocol_t::build_mode_t>(m__io->read_u1());
}

nix_daemon_protocol_t::build_paths_with_results_request_t::~build_paths_with_results_request_t() {
  _clean_up();
}

void nix_daemon_protocol_t::build_paths_with_results_request_t::_clean_up() {}

nix_daemon_protocol_t::error_trace_t::error_trace_t(kaitai::kstream* p__io,
                                                    kaitai::kstruct* p__parent,
                                                    nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  m_msg = nullptr;
  _read();
}

void nix_daemon_protocol_t::error_trace_t::_read() {
  m_have_pos = m__io->read_u8le();
  m_msg = std::unique_ptr<nix_string_t>(new nix_string_t(m__io, this, m__root));
}

nix_daemon_protocol_t::error_trace_t::~error_trace_t() {
  _clean_up();
}

void nix_daemon_protocol_t::error_trace_t::_clean_up() {}

nix_daemon_protocol_t::set_options_request_t::set_options_request_t(kaitai::kstream* p__io,
                                                                    kaitai::kstruct* p__parent,
                                                                    nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  m_overrides = nullptr;
  _read();
}

void nix_daemon_protocol_t::set_options_request_t::_read() {
  m_keep_failed = m__io->read_u8le();
  m_keep_going = m__io->read_u8le();
  m_try_fallback = m__io->read_u8le();
  m_verbosity = m__io->read_u8le();
  m_max_build_jobs = m__io->read_u8le();
  m_max_silent_time = m__io->read_u8le();
  m_obsolete_use_build_hook = m__io->read_u8le();
  m_verbose_build = m__io->read_u8le();
  m_obsolete_log_type = m__io->read_u8le();
  m_obsolete_print_build_trace = m__io->read_u8le();
  m_build_cores = m__io->read_u8le();
  m_use_substitutes = m__io->read_u8le();
  n_overrides = true;
  if (_root()->protocol_version() >= 12) {
    n_overrides = false;
    m_overrides =
        std::unique_ptr<setting_overrides_t>(new setting_overrides_t(m__io, this, m__root));
  }
}

nix_daemon_protocol_t::set_options_request_t::~set_options_request_t() {
  _clean_up();
}

void nix_daemon_protocol_t::set_options_request_t::_clean_up() {
  if (!n_overrides) {
  }
}

nix_daemon_protocol_t::query_realisation_response_t::query_realisation_response_t(
    kaitai::kstream* p__io, kaitai::kstruct* p__parent, nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  m_realisations = nullptr;
  _read();
}

void nix_daemon_protocol_t::query_realisation_response_t::_read() {
  m_num_realisations = m__io->read_u8le();
  m_realisations = std::unique_ptr<std::vector<std::unique_ptr<realisation_t>>>(
      new std::vector<std::unique_ptr<realisation_t>>());
  const int l_realisations = num_realisations();
  for (int i = 0; i < l_realisations; i++) {
    m_realisations->push_back(
        std::move(std::unique_ptr<realisation_t>(new realisation_t(m__io, this, m__root))));
  }
}

nix_daemon_protocol_t::query_realisation_response_t::~query_realisation_response_t() {
  _clean_up();
}

void nix_daemon_protocol_t::query_realisation_response_t::_clean_up() {}

nix_daemon_protocol_t::query_substitutable_path_info_request_t::
    query_substitutable_path_info_request_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent,
                                            nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  m_path = nullptr;
  _read();
}

void nix_daemon_protocol_t::query_substitutable_path_info_request_t::_read() {
  m_path = std::unique_ptr<store_path_t>(new store_path_t(m__io, this, m__root));
}

nix_daemon_protocol_t::query_substitutable_path_info_request_t::
    ~query_substitutable_path_info_request_t() {
  _clean_up();
}

void nix_daemon_protocol_t::query_substitutable_path_info_request_t::_clean_up() {}

nix_daemon_protocol_t::query_derivation_output_map_request_t::query_derivation_output_map_request_t(
    kaitai::kstream* p__io, kaitai::kstruct* p__parent, nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  m_path = nullptr;
  _read();
}

void nix_daemon_protocol_t::query_derivation_output_map_request_t::_read() {
  m_path = std::unique_ptr<store_path_t>(new store_path_t(m__io, this, m__root));
}

nix_daemon_protocol_t::query_derivation_output_map_request_t::
    ~query_derivation_output_map_request_t() {
  _clean_up();
}

void nix_daemon_protocol_t::query_derivation_output_map_request_t::_clean_up() {}

nix_daemon_protocol_t::build_derivation_request_t::build_derivation_request_t(
    kaitai::kstream* p__io, kaitai::kstruct* p__parent, nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  m_drv_path = nullptr;
  m_derivation = nullptr;
  _read();
}

void nix_daemon_protocol_t::build_derivation_request_t::_read() {
  m_drv_path = std::unique_ptr<store_path_t>(new store_path_t(m__io, this, m__root));
  m_derivation = std::unique_ptr<nix_bytes_t>(new nix_bytes_t(m__io, this, m__root));
  m_build_mode = static_cast<nix_daemon_protocol_t::build_mode_t>(m__io->read_u1());
}

nix_daemon_protocol_t::build_derivation_request_t::~build_derivation_request_t() {
  _clean_up();
}

void nix_daemon_protocol_t::build_derivation_request_t::_clean_up() {}

nix_daemon_protocol_t::stderr_next_t::stderr_next_t(kaitai::kstream* p__io,
                                                    kaitai::kstruct* p__parent,
                                                    nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  m_msg = nullptr;
  _read();
}

void nix_daemon_protocol_t::stderr_next_t::_read() {
  m_msg = std::unique_ptr<nix_string_t>(new nix_string_t(m__io, this, m__root));
}

nix_daemon_protocol_t::stderr_next_t::~stderr_next_t() {
  _clean_up();
}

void nix_daemon_protocol_t::stderr_next_t::_clean_up() {}

nix_daemon_protocol_t::derivation_output_map_t::derivation_output_map_t(
    kaitai::kstream* p__io, kaitai::kstruct* p__parent, nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  m_outputs = nullptr;
  _read();
}

void nix_daemon_protocol_t::derivation_output_map_t::_read() {
  m_num_outputs = m__io->read_u8le();
  m_outputs = std::unique_ptr<std::vector<std::unique_ptr<derivation_output_entry_t>>>(
      new std::vector<std::unique_ptr<derivation_output_entry_t>>());
  const int l_outputs = num_outputs();
  for (int i = 0; i < l_outputs; i++) {
    m_outputs->push_back(std::move(std::unique_ptr<derivation_output_entry_t>(
        new derivation_output_entry_t(m__io, this, m__root))));
  }
}

nix_daemon_protocol_t::derivation_output_map_t::~derivation_output_map_t() {
  _clean_up();
}

void nix_daemon_protocol_t::derivation_output_map_t::_clean_up() {}

nix_daemon_protocol_t::empty_request_t::empty_request_t(kaitai::kstream* p__io,
                                                        kaitai::kstruct* p__parent,
                                                        nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  _read();
}

void nix_daemon_protocol_t::empty_request_t::_read() {}

nix_daemon_protocol_t::empty_request_t::~empty_request_t() {
  _clean_up();
}

void nix_daemon_protocol_t::empty_request_t::_clean_up() {}

nix_daemon_protocol_t::add_perm_root_request_t::add_perm_root_request_t(
    kaitai::kstream* p__io, kaitai::kstruct* p__parent, nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  m_store_path = nullptr;
  m_gc_root = nullptr;
  _read();
}

void nix_daemon_protocol_t::add_perm_root_request_t::_read() {
  m_store_path = std::unique_ptr<store_path_t>(new store_path_t(m__io, this, m__root));
  m_gc_root = std::unique_ptr<nix_string_t>(new nix_string_t(m__io, this, m__root));
}

nix_daemon_protocol_t::add_perm_root_request_t::~add_perm_root_request_t() {
  _clean_up();
}

void nix_daemon_protocol_t::add_perm_root_request_t::_clean_up() {}

nix_daemon_protocol_t::query_substitutable_path_info_response_t::
    query_substitutable_path_info_response_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent,
                                             nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  m_deriver = nullptr;
  m_references = nullptr;
  _read();
}

void nix_daemon_protocol_t::query_substitutable_path_info_response_t::_read() {
  m_found = m__io->read_u8le();
  n_deriver = true;
  if (found() != 0) {
    n_deriver = false;
    m_deriver =
        std::unique_ptr<optional_store_path_t>(new optional_store_path_t(m__io, this, m__root));
  }
  n_references = true;
  if (found() != 0) {
    n_references = false;
    m_references = std::unique_ptr<store_path_set_t>(new store_path_set_t(m__io, this, m__root));
  }
  n_download_size = true;
  if (found() != 0) {
    n_download_size = false;
    m_download_size = m__io->read_u8le();
  }
  n_nar_size = true;
  if (found() != 0) {
    n_nar_size = false;
    m_nar_size = m__io->read_u8le();
  }
}

nix_daemon_protocol_t::query_substitutable_path_info_response_t::
    ~query_substitutable_path_info_response_t() {
  _clean_up();
}

void nix_daemon_protocol_t::query_substitutable_path_info_response_t::_clean_up() {
  if (!n_deriver) {
  }
  if (!n_references) {
  }
  if (!n_download_size) {
  }
  if (!n_nar_size) {
  }
}

nix_daemon_protocol_t::query_path_from_hash_part_request_t::query_path_from_hash_part_request_t(
    kaitai::kstream* p__io, kaitai::kstruct* p__parent, nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  m_hash_part = nullptr;
  _read();
}

void nix_daemon_protocol_t::query_path_from_hash_part_request_t::_read() {
  m_hash_part = std::unique_ptr<nix_string_t>(new nix_string_t(m__io, this, m__root));
}

nix_daemon_protocol_t::query_path_from_hash_part_request_t::~query_path_from_hash_part_request_t() {
  _clean_up();
}

void nix_daemon_protocol_t::query_path_from_hash_part_request_t::_clean_up() {}

nix_daemon_protocol_t::derived_path_list_t::derived_path_list_t(kaitai::kstream* p__io,
                                                                kaitai::kstruct* p__parent,
                                                                nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  m_paths = nullptr;
  _read();
}

void nix_daemon_protocol_t::derived_path_list_t::_read() {
  m_num_paths = m__io->read_u8le();
  m_paths = std::unique_ptr<std::vector<std::unique_ptr<derived_path_t>>>(
      new std::vector<std::unique_ptr<derived_path_t>>());
  const int l_paths = num_paths();
  for (int i = 0; i < l_paths; i++) {
    m_paths->push_back(
        std::move(std::unique_ptr<derived_path_t>(new derived_path_t(m__io, this, m__root))));
  }
}

nix_daemon_protocol_t::derived_path_list_t::~derived_path_list_t() {
  _clean_up();
}

void nix_daemon_protocol_t::derived_path_list_t::_clean_up() {}

nix_daemon_protocol_t::derived_path_t::derived_path_t(kaitai::kstream* p__io,
                                                      kaitai::kstruct* p__parent,
                                                      nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  m_path = nullptr;
  _read();
}

void nix_daemon_protocol_t::derived_path_t::_read() {
  m_path = std::unique_ptr<nix_string_t>(new nix_string_t(m__io, this, m__root));
}

nix_daemon_protocol_t::derived_path_t::~derived_path_t() {
  _clean_up();
}

void nix_daemon_protocol_t::derived_path_t::_clean_up() {}

nix_daemon_protocol_t::query_missing_request_t::query_missing_request_t(
    kaitai::kstream* p__io, kaitai::kstruct* p__parent, nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  m_targets = nullptr;
  _read();
}

void nix_daemon_protocol_t::query_missing_request_t::_read() {
  m_targets = std::unique_ptr<derived_path_list_t>(new derived_path_list_t(m__io, this, m__root));
}

nix_daemon_protocol_t::query_missing_request_t::~query_missing_request_t() {
  _clean_up();
}

void nix_daemon_protocol_t::query_missing_request_t::_clean_up() {}

nix_daemon_protocol_t::query_valid_derivers_request_t::query_valid_derivers_request_t(
    kaitai::kstream* p__io, kaitai::kstruct* p__parent, nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  m_path = nullptr;
  _read();
}

void nix_daemon_protocol_t::query_valid_derivers_request_t::_read() {
  m_path = std::unique_ptr<store_path_t>(new store_path_t(m__io, this, m__root));
}

nix_daemon_protocol_t::query_valid_derivers_request_t::~query_valid_derivers_request_t() {
  _clean_up();
}

void nix_daemon_protocol_t::query_valid_derivers_request_t::_clean_up() {}

nix_daemon_protocol_t::unkeyed_valid_path_info_t::unkeyed_valid_path_info_t(
    kaitai::kstream* p__io, kaitai::kstruct* p__parent, nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  m_deriver = nullptr;
  m_nar_hash = nullptr;
  m_references = nullptr;
  m_sigs = nullptr;
  m_ca = nullptr;
  _read();
}

void nix_daemon_protocol_t::unkeyed_valid_path_info_t::_read() {
  m_deriver =
      std::unique_ptr<optional_store_path_t>(new optional_store_path_t(m__io, this, m__root));
  m_nar_hash = std::unique_ptr<nix_string_t>(new nix_string_t(m__io, this, m__root));
  m_references = std::unique_ptr<store_path_set_t>(new store_path_set_t(m__io, this, m__root));
  m_registration_time = m__io->read_u8le();
  m_nar_size = m__io->read_u8le();
  n_ultimate = true;
  if (_root()->protocol_version() >= 16) {
    n_ultimate = false;
    m_ultimate = m__io->read_u8le();
  }
  n_sigs = true;
  if (_root()->protocol_version() >= 16) {
    n_sigs = false;
    m_sigs = std::unique_ptr<nix_string_set_t>(new nix_string_set_t(m__io, this, m__root));
  }
  n_ca = true;
  if (_root()->protocol_version() >= 16) {
    n_ca = false;
    m_ca = std::unique_ptr<nix_string_t>(new nix_string_t(m__io, this, m__root));
  }
}

nix_daemon_protocol_t::unkeyed_valid_path_info_t::~unkeyed_valid_path_info_t() {
  _clean_up();
}

void nix_daemon_protocol_t::unkeyed_valid_path_info_t::_clean_up() {
  if (!n_ultimate) {
  }
  if (!n_sigs) {
  }
  if (!n_ca) {
  }
}

nix_daemon_protocol_t::feature_exchange_t::feature_exchange_t(kaitai::kstream* p__io,
                                                              kaitai::kstruct* p__parent,
                                                              nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  m_features = nullptr;
  _read();
}

void nix_daemon_protocol_t::feature_exchange_t::_read() {
  m_features = std::unique_ptr<nix_string_set_t>(new nix_string_set_t(m__io, this, m__root));
}

nix_daemon_protocol_t::feature_exchange_t::~feature_exchange_t() {
  _clean_up();
}

void nix_daemon_protocol_t::feature_exchange_t::_clean_up() {}

nix_daemon_protocol_t::nix_string_list_t::nix_string_list_t(kaitai::kstream* p__io,
                                                            kaitai::kstruct* p__parent,
                                                            nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  m_items = nullptr;
  _read();
}

void nix_daemon_protocol_t::nix_string_list_t::_read() {
  m_num_items = m__io->read_u8le();
  m_items = std::unique_ptr<std::vector<std::unique_ptr<nix_string_t>>>(
      new std::vector<std::unique_ptr<nix_string_t>>());
  const int l_items = num_items();
  for (int i = 0; i < l_items; i++) {
    m_items->push_back(
        std::move(std::unique_ptr<nix_string_t>(new nix_string_t(m__io, this, m__root))));
  }
}

nix_daemon_protocol_t::nix_string_list_t::~nix_string_list_t() {
  _clean_up();
}

void nix_daemon_protocol_t::nix_string_list_t::_clean_up() {}

nix_daemon_protocol_t::error_trace_list_t::error_trace_list_t(kaitai::kstream* p__io,
                                                              kaitai::kstruct* p__parent,
                                                              nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  m_traces = nullptr;
  _read();
}

void nix_daemon_protocol_t::error_trace_list_t::_read() {
  m_num_traces = m__io->read_u8le();
  m_traces = std::unique_ptr<std::vector<std::unique_ptr<error_trace_t>>>(
      new std::vector<std::unique_ptr<error_trace_t>>());
  const int l_traces = num_traces();
  for (int i = 0; i < l_traces; i++) {
    m_traces->push_back(
        std::move(std::unique_ptr<error_trace_t>(new error_trace_t(m__io, this, m__root))));
  }
}

nix_daemon_protocol_t::error_trace_list_t::~error_trace_list_t() {
  _clean_up();
}

void nix_daemon_protocol_t::error_trace_list_t::_clean_up() {}

nix_daemon_protocol_t::query_referrers_request_t::query_referrers_request_t(
    kaitai::kstream* p__io, kaitai::kstruct* p__parent, nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  m_path = nullptr;
  _read();
}

void nix_daemon_protocol_t::query_referrers_request_t::_read() {
  m_path = std::unique_ptr<store_path_t>(new store_path_t(m__io, this, m__root));
}

nix_daemon_protocol_t::query_referrers_request_t::~query_referrers_request_t() {
  _clean_up();
}

void nix_daemon_protocol_t::query_referrers_request_t::_clean_up() {}

nix_daemon_protocol_t::nix_bytes_t::nix_bytes_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent,
                                                nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  _read();
}

void nix_daemon_protocol_t::nix_bytes_t::_read() {
  m_len = m__io->read_u8le();
  m_data = m__io->read_bytes(len());
  m_padding = m__io->read_bytes(kaitai::kstream::mod((8 - kaitai::kstream::mod(len(), 8)), 8));
}

nix_daemon_protocol_t::nix_bytes_t::~nix_bytes_t() {
  _clean_up();
}

void nix_daemon_protocol_t::nix_bytes_t::_clean_up() {}

nix_daemon_protocol_t::query_derivation_output_names_request_t::
    query_derivation_output_names_request_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent,
                                            nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  m_path = nullptr;
  _read();
}

void nix_daemon_protocol_t::query_derivation_output_names_request_t::_read() {
  m_path = std::unique_ptr<store_path_t>(new store_path_t(m__io, this, m__root));
}

nix_daemon_protocol_t::query_derivation_output_names_request_t::
    ~query_derivation_output_names_request_t() {
  _clean_up();
}

void nix_daemon_protocol_t::query_derivation_output_names_request_t::_clean_up() {}

nix_daemon_protocol_t::verify_store_request_t::verify_store_request_t(
    kaitai::kstream* p__io, kaitai::kstruct* p__parent, nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  _read();
}

void nix_daemon_protocol_t::verify_store_request_t::_read() {
  m_check_contents = m__io->read_u8le();
  m_repair = m__io->read_u8le();
}

nix_daemon_protocol_t::verify_store_request_t::~verify_store_request_t() {
  _clean_up();
}

void nix_daemon_protocol_t::verify_store_request_t::_clean_up() {}

nix_daemon_protocol_t::server_hello_t::server_hello_t(kaitai::kstream* p__io,
                                                      kaitai::kstruct* p__parent,
                                                      nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  _read();
}

void nix_daemon_protocol_t::server_hello_t::_read() {
  m_magic = m__io->read_bytes(4);
  if (!(magic() == std::string("\x6F\x69\x78\x64", 4))) {
    throw kaitai::validation_not_equal_error<std::string>(std::string("\x6F\x69\x78\x64", 4),
                                                          magic(), _io(),
                                                          std::string("/types/server_hello/seq/0"));
  }
  m_padding1 = m__io->read_bytes(4);
  if (!(padding1() == std::string("\x00\x00\x00\x00", 4))) {
    throw kaitai::validation_not_equal_error<std::string>(std::string("\x00\x00\x00\x00", 4),
                                                          padding1(), _io(),
                                                          std::string("/types/server_hello/seq/1"));
  }
  m_server_version = m__io->read_u8le();
}

nix_daemon_protocol_t::server_hello_t::~server_hello_t() {
  _clean_up();
}

void nix_daemon_protocol_t::server_hello_t::_clean_up() {}

nix_daemon_protocol_t::query_path_info_response_t::query_path_info_response_t(
    kaitai::kstream* p__io, kaitai::kstruct* p__parent, nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  m_info = nullptr;
  _read();
}

void nix_daemon_protocol_t::query_path_info_response_t::_read() {
  m_valid = m__io->read_u8le();
  n_info = true;
  if (valid() != 0) {
    n_info = false;
    m_info = std::unique_ptr<unkeyed_valid_path_info_t>(
        new unkeyed_valid_path_info_t(m__io, this, m__root));
  }
}

nix_daemon_protocol_t::query_path_info_response_t::~query_path_info_response_t() {
  _clean_up();
}

void nix_daemon_protocol_t::query_path_info_response_t::_clean_up() {
  if (!n_info) {
  }
}

nix_daemon_protocol_t::client_hello_t::client_hello_t(kaitai::kstream* p__io,
                                                      kaitai::kstruct* p__parent,
                                                      nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  _read();
}

void nix_daemon_protocol_t::client_hello_t::_read() {
  m_magic = m__io->read_bytes(4);
  if (!(magic() == std::string("\x63\x78\x69\x6E", 4))) {
    throw kaitai::validation_not_equal_error<std::string>(std::string("\x63\x78\x69\x6E", 4),
                                                          magic(), _io(),
                                                          std::string("/types/client_hello/seq/0"));
  }
  m_padding1 = m__io->read_bytes(4);
  if (!(padding1() == std::string("\x00\x00\x00\x00", 4))) {
    throw kaitai::validation_not_equal_error<std::string>(std::string("\x00\x00\x00\x00", 4),
                                                          padding1(), _io(),
                                                          std::string("/types/client_hello/seq/1"));
  }
  m_client_version = m__io->read_u8le();
}

nix_daemon_protocol_t::client_hello_t::~client_hello_t() {
  _clean_up();
}

void nix_daemon_protocol_t::client_hello_t::_clean_up() {}

nix_daemon_protocol_t::query_realisation_request_t::query_realisation_request_t(
    kaitai::kstream* p__io, kaitai::kstruct* p__parent, nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  m_output_id = nullptr;
  _read();
}

void nix_daemon_protocol_t::query_realisation_request_t::_read() {
  m_output_id = std::unique_ptr<drv_output_t>(new drv_output_t(m__io, this, m__root));
}

nix_daemon_protocol_t::query_realisation_request_t::~query_realisation_request_t() {
  _clean_up();
}

void nix_daemon_protocol_t::query_realisation_request_t::_clean_up() {}

nix_daemon_protocol_t::stderr_error_t::stderr_error_t(kaitai::kstream* p__io,
                                                      kaitai::kstruct* p__parent,
                                                      nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  m_error = nullptr;
  m_error_msg = nullptr;
  _read();
}

void nix_daemon_protocol_t::stderr_error_t::_read() {
  n_error = true;
  if (_root()->protocol_version() >= 26) {
    n_error = false;
    m_error = std::unique_ptr<nix_error_t>(new nix_error_t(m__io, this, m__root));
  }
  n_error_msg = true;
  if (_root()->protocol_version() < 26) {
    n_error_msg = false;
    m_error_msg = std::unique_ptr<nix_string_t>(new nix_string_t(m__io, this, m__root));
  }
  n_status = true;
  if (_root()->protocol_version() < 26) {
    n_status = false;
    m_status = m__io->read_u8le();
  }
}

nix_daemon_protocol_t::stderr_error_t::~stderr_error_t() {
  _clean_up();
}

void nix_daemon_protocol_t::stderr_error_t::_clean_up() {
  if (!n_error) {
  }
  if (!n_error_msg) {
  }
  if (!n_status) {
  }
}

nix_daemon_protocol_t::build_paths_with_results_response_t::build_paths_with_results_response_t(
    kaitai::kstream* p__io, kaitai::kstruct* p__parent, nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  m_results = nullptr;
  _read();
}

void nix_daemon_protocol_t::build_paths_with_results_response_t::_read() {
  m_num_results = m__io->read_u8le();
  m_results = std::unique_ptr<std::vector<std::unique_ptr<keyed_build_result_t>>>(
      new std::vector<std::unique_ptr<keyed_build_result_t>>());
  const int l_results = num_results();
  for (int i = 0; i < l_results; i++) {
    m_results->push_back(std::move(
        std::unique_ptr<keyed_build_result_t>(new keyed_build_result_t(m__io, this, m__root))));
  }
}

nix_daemon_protocol_t::build_paths_with_results_response_t::~build_paths_with_results_response_t() {
  _clean_up();
}

void nix_daemon_protocol_t::build_paths_with_results_response_t::_clean_up() {}

nix_daemon_protocol_t::ensure_path_request_t::ensure_path_request_t(kaitai::kstream* p__io,
                                                                    kaitai::kstruct* p__parent,
                                                                    nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  m_path = nullptr;
  _read();
}

void nix_daemon_protocol_t::ensure_path_request_t::_read() {
  m_path = std::unique_ptr<store_path_t>(new store_path_t(m__io, this, m__root));
}

nix_daemon_protocol_t::ensure_path_request_t::~ensure_path_request_t() {
  _clean_up();
}

void nix_daemon_protocol_t::ensure_path_request_t::_clean_up() {}

nix_daemon_protocol_t::root_entry_t::root_entry_t(kaitai::kstream* p__io,
                                                  kaitai::kstruct* p__parent,
                                                  nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  m_link = nullptr;
  m_target = nullptr;
  _read();
}

void nix_daemon_protocol_t::root_entry_t::_read() {
  m_link = std::unique_ptr<nix_string_t>(new nix_string_t(m__io, this, m__root));
  m_target = std::unique_ptr<store_path_t>(new store_path_t(m__io, this, m__root));
}

nix_daemon_protocol_t::root_entry_t::~root_entry_t() {
  _clean_up();
}

void nix_daemon_protocol_t::root_entry_t::_clean_up() {}

nix_daemon_protocol_t::logger_field_t::logger_field_t(kaitai::kstream* p__io,
                                                      kaitai::kstruct* p__parent,
                                                      nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  m_string_value = nullptr;
  _read();
}

void nix_daemon_protocol_t::logger_field_t::_read() {
  m_field_type = static_cast<nix_daemon_protocol_t::logger_field_type_t>(m__io->read_u8le());
  n_int_value = true;
  if (field_type() == nix_daemon_protocol_t::LOGGER_FIELD_TYPE_INT) {
    n_int_value = false;
    m_int_value = m__io->read_u8le();
  }
  n_string_value = true;
  if (field_type() == nix_daemon_protocol_t::LOGGER_FIELD_TYPE_STRING) {
    n_string_value = false;
    m_string_value = std::unique_ptr<nix_string_t>(new nix_string_t(m__io, this, m__root));
  }
}

nix_daemon_protocol_t::logger_field_t::~logger_field_t() {
  _clean_up();
}

void nix_daemon_protocol_t::logger_field_t::_clean_up() {
  if (!n_int_value) {
  }
  if (!n_string_value) {
  }
}

nix_daemon_protocol_t::stderr_write_t::stderr_write_t(kaitai::kstream* p__io,
                                                      kaitai::kstruct* p__parent,
                                                      nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  m_data = nullptr;
  _read();
}

void nix_daemon_protocol_t::stderr_write_t::_read() {
  m_data = std::unique_ptr<nix_string_t>(new nix_string_t(m__io, this, m__root));
}

nix_daemon_protocol_t::stderr_write_t::~stderr_write_t() {
  _clean_up();
}

void nix_daemon_protocol_t::stderr_write_t::_clean_up() {}

nix_daemon_protocol_t::query_deriver_request_t::query_deriver_request_t(
    kaitai::kstream* p__io, kaitai::kstruct* p__parent, nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  m_path = nullptr;
  _read();
}

void nix_daemon_protocol_t::query_deriver_request_t::_read() {
  m_path = std::unique_ptr<store_path_t>(new store_path_t(m__io, this, m__root));
}

nix_daemon_protocol_t::query_deriver_request_t::~query_deriver_request_t() {
  _clean_up();
}

void nix_daemon_protocol_t::query_deriver_request_t::_clean_up() {}

nix_daemon_protocol_t::query_derivation_outputs_request_t::query_derivation_outputs_request_t(
    kaitai::kstream* p__io, kaitai::kstruct* p__parent, nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  m_path = nullptr;
  _read();
}

void nix_daemon_protocol_t::query_derivation_outputs_request_t::_read() {
  m_path = std::unique_ptr<store_path_t>(new store_path_t(m__io, this, m__root));
}

nix_daemon_protocol_t::query_derivation_outputs_request_t::~query_derivation_outputs_request_t() {
  _clean_up();
}

void nix_daemon_protocol_t::query_derivation_outputs_request_t::_clean_up() {}

nix_daemon_protocol_t::query_realisation_response_old_t::query_realisation_response_old_t(
    kaitai::kstream* p__io, kaitai::kstruct* p__parent, nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  m_out_paths = nullptr;
  _read();
}

void nix_daemon_protocol_t::query_realisation_response_old_t::_read() {
  m_out_paths = std::unique_ptr<store_path_set_t>(new store_path_set_t(m__io, this, m__root));
}

nix_daemon_protocol_t::query_realisation_response_old_t::~query_realisation_response_old_t() {
  _clean_up();
}

void nix_daemon_protocol_t::query_realisation_response_old_t::_clean_up() {}

nix_daemon_protocol_t::collect_garbage_request_t::collect_garbage_request_t(
    kaitai::kstream* p__io, kaitai::kstruct* p__parent, nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  m_paths_to_delete = nullptr;
  _read();
}

void nix_daemon_protocol_t::collect_garbage_request_t::_read() {
  m_action = static_cast<nix_daemon_protocol_t::gc_action_t>(m__io->read_u8le());
  m_paths_to_delete = std::unique_ptr<store_path_set_t>(new store_path_set_t(m__io, this, m__root));
  m_ignore_liveness = m__io->read_u8le();
  m_max_freed = m__io->read_u8le();
  m_obsolete1 = m__io->read_u8le();
  m_obsolete2 = m__io->read_u8le();
  m_obsolete3 = m__io->read_u8le();
}

nix_daemon_protocol_t::collect_garbage_request_t::~collect_garbage_request_t() {
  _clean_up();
}

void nix_daemon_protocol_t::collect_garbage_request_t::_clean_up() {}

nix_daemon_protocol_t::substitutable_path_info_entry_t::substitutable_path_info_entry_t(
    kaitai::kstream* p__io, kaitai::kstruct* p__parent, nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  m_path = nullptr;
  m_deriver = nullptr;
  m_references = nullptr;
  _read();
}

void nix_daemon_protocol_t::substitutable_path_info_entry_t::_read() {
  m_path = std::unique_ptr<store_path_t>(new store_path_t(m__io, this, m__root));
  m_deriver =
      std::unique_ptr<optional_store_path_t>(new optional_store_path_t(m__io, this, m__root));
  m_references = std::unique_ptr<store_path_set_t>(new store_path_set_t(m__io, this, m__root));
  m_download_size = m__io->read_u8le();
  m_nar_size = m__io->read_u8le();
}

nix_daemon_protocol_t::substitutable_path_info_entry_t::~substitutable_path_info_entry_t() {
  _clean_up();
}

void nix_daemon_protocol_t::substitutable_path_info_entry_t::_clean_up() {}

nix_daemon_protocol_t::query_path_info_request_t::query_path_info_request_t(
    kaitai::kstream* p__io, kaitai::kstruct* p__parent, nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  m_path = nullptr;
  _read();
}

void nix_daemon_protocol_t::query_path_info_request_t::_read() {
  m_path = std::unique_ptr<store_path_t>(new store_path_t(m__io, this, m__root));
}

nix_daemon_protocol_t::query_path_info_request_t::~query_path_info_request_t() {
  _clean_up();
}

void nix_daemon_protocol_t::query_path_info_request_t::_clean_up() {}

nix_daemon_protocol_t::keyed_build_result_t::keyed_build_result_t(kaitai::kstream* p__io,
                                                                  kaitai::kstruct* p__parent,
                                                                  nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  m_path = nullptr;
  m_result = nullptr;
  _read();
}

void nix_daemon_protocol_t::keyed_build_result_t::_read() {
  m_path = std::unique_ptr<derived_path_t>(new derived_path_t(m__io, this, m__root));
  m_result = std::unique_ptr<build_result_t>(new build_result_t(m__io, this, m__root));
}

nix_daemon_protocol_t::keyed_build_result_t::~keyed_build_result_t() {
  _clean_up();
}

void nix_daemon_protocol_t::keyed_build_result_t::_clean_up() {}

nix_daemon_protocol_t::drv_output_entry_t::drv_output_entry_t(kaitai::kstream* p__io,
                                                              kaitai::kstruct* p__parent,
                                                              nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  m_output_id = nullptr;
  m_realisation = nullptr;
  _read();
}

void nix_daemon_protocol_t::drv_output_entry_t::_read() {
  m_output_id = std::unique_ptr<drv_output_t>(new drv_output_t(m__io, this, m__root));
  m_realisation = std::unique_ptr<realisation_t>(new realisation_t(m__io, this, m__root));
}

nix_daemon_protocol_t::drv_output_entry_t::~drv_output_entry_t() {
  _clean_up();
}

void nix_daemon_protocol_t::drv_output_entry_t::_clean_up() {}

nix_daemon_protocol_t::optional_store_path_t::optional_store_path_t(kaitai::kstream* p__io,
                                                                    kaitai::kstruct* p__parent,
                                                                    nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  m_path = nullptr;
  f_is_present = false;
  _read();
}

void nix_daemon_protocol_t::optional_store_path_t::_read() {
  m_path = std::unique_ptr<nix_string_t>(new nix_string_t(m__io, this, m__root));
}

nix_daemon_protocol_t::optional_store_path_t::~optional_store_path_t() {
  _clean_up();
}

void nix_daemon_protocol_t::optional_store_path_t::_clean_up() {}

bool nix_daemon_protocol_t::optional_store_path_t::is_present() {
  if (f_is_present)
    return m_is_present;
  m_is_present = path()->len() > 0;
  f_is_present = true;
  return m_is_present;
}

nix_daemon_protocol_t::build_result_t::build_result_t(kaitai::kstream* p__io,
                                                      kaitai::kstruct* p__parent,
                                                      nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  m_error_msg = nullptr;
  m_cpu_user = nullptr;
  m_cpu_system = nullptr;
  m_built_outputs = nullptr;
  _read();
}

void nix_daemon_protocol_t::build_result_t::_read() {
  m_status = static_cast<nix_daemon_protocol_t::build_status_t>(m__io->read_u8le());
  m_error_msg = std::unique_ptr<nix_string_t>(new nix_string_t(m__io, this, m__root));
  n_times_built = true;
  if (_root()->protocol_version() >= 29) {
    n_times_built = false;
    m_times_built = m__io->read_u8le();
  }
  n_is_non_deterministic = true;
  if (_root()->protocol_version() >= 29) {
    n_is_non_deterministic = false;
    m_is_non_deterministic = m__io->read_u8le();
  }
  n_start_time = true;
  if (_root()->protocol_version() >= 29) {
    n_start_time = false;
    m_start_time = m__io->read_u8le();
  }
  n_stop_time = true;
  if (_root()->protocol_version() >= 29) {
    n_stop_time = false;
    m_stop_time = m__io->read_u8le();
  }
  n_cpu_user = true;
  if (_root()->protocol_version() >= 37) {
    n_cpu_user = false;
    m_cpu_user =
        std::unique_ptr<optional_duration_t>(new optional_duration_t(m__io, this, m__root));
  }
  n_cpu_system = true;
  if (_root()->protocol_version() >= 37) {
    n_cpu_system = false;
    m_cpu_system =
        std::unique_ptr<optional_duration_t>(new optional_duration_t(m__io, this, m__root));
  }
  n_built_outputs = true;
  if (_root()->protocol_version() >= 28) {
    n_built_outputs = false;
    m_built_outputs = std::unique_ptr<drv_outputs_t>(new drv_outputs_t(m__io, this, m__root));
  }
}

nix_daemon_protocol_t::build_result_t::~build_result_t() {
  _clean_up();
}

void nix_daemon_protocol_t::build_result_t::_clean_up() {
  if (!n_times_built) {
  }
  if (!n_is_non_deterministic) {
  }
  if (!n_start_time) {
  }
  if (!n_stop_time) {
  }
  if (!n_cpu_user) {
  }
  if (!n_cpu_system) {
  }
  if (!n_built_outputs) {
  }
}

nix_daemon_protocol_t::stderr_read_t::stderr_read_t(kaitai::kstream* p__io,
                                                    kaitai::kstruct* p__parent,
                                                    nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  _read();
}

void nix_daemon_protocol_t::stderr_read_t::_read() {
  m_len = m__io->read_u8le();
}

nix_daemon_protocol_t::stderr_read_t::~stderr_read_t() {
  _clean_up();
}

void nix_daemon_protocol_t::stderr_read_t::_clean_up() {}

nix_daemon_protocol_t::query_path_hash_request_t::query_path_hash_request_t(
    kaitai::kstream* p__io, kaitai::kstruct* p__parent, nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  m_path = nullptr;
  _read();
}

void nix_daemon_protocol_t::query_path_hash_request_t::_read() {
  m_path = std::unique_ptr<store_path_t>(new store_path_t(m__io, this, m__root));
}

nix_daemon_protocol_t::query_path_hash_request_t::~query_path_hash_request_t() {
  _clean_up();
}

void nix_daemon_protocol_t::query_path_hash_request_t::_clean_up() {}

nix_daemon_protocol_t::bool_response_t::bool_response_t(kaitai::kstream* p__io,
                                                        kaitai::kstruct* p__parent,
                                                        nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  _read();
}

void nix_daemon_protocol_t::bool_response_t::_read() {
  m_value = m__io->read_u8le();
}

nix_daemon_protocol_t::bool_response_t::~bool_response_t() {
  _clean_up();
}

void nix_daemon_protocol_t::bool_response_t::_clean_up() {}

nix_daemon_protocol_t::build_paths_request_t::build_paths_request_t(kaitai::kstream* p__io,
                                                                    kaitai::kstruct* p__parent,
                                                                    nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  m_paths = nullptr;
  _read();
}

void nix_daemon_protocol_t::build_paths_request_t::_read() {
  m_paths = std::unique_ptr<derived_path_list_t>(new derived_path_list_t(m__io, this, m__root));
  m_build_mode = static_cast<nix_daemon_protocol_t::build_mode_t>(m__io->read_u1());
}

nix_daemon_protocol_t::build_paths_request_t::~build_paths_request_t() {
  _clean_up();
}

void nix_daemon_protocol_t::build_paths_request_t::_clean_up() {}

nix_daemon_protocol_t::stderr_last_t::stderr_last_t(kaitai::kstream* p__io,
                                                    kaitai::kstruct* p__parent,
                                                    nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  _read();
}

void nix_daemon_protocol_t::stderr_last_t::_read() {}

nix_daemon_protocol_t::stderr_last_t::~stderr_last_t() {
  _clean_up();
}

void nix_daemon_protocol_t::stderr_last_t::_clean_up() {}

nix_daemon_protocol_t::add_multiple_to_store_request_t::add_multiple_to_store_request_t(
    kaitai::kstream* p__io, kaitai::kstruct* p__parent, nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  _read();
}

void nix_daemon_protocol_t::add_multiple_to_store_request_t::_read() {
  m_repair = m__io->read_u8le();
  m_dont_check_sigs = m__io->read_u8le();
}

nix_daemon_protocol_t::add_multiple_to_store_request_t::~add_multiple_to_store_request_t() {
  _clean_up();
}

void nix_daemon_protocol_t::add_multiple_to_store_request_t::_clean_up() {}

nix_daemon_protocol_t::store_path_ca_map_t::store_path_ca_map_t(kaitai::kstream* p__io,
                                                                kaitai::kstruct* p__parent,
                                                                nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  m_entries = nullptr;
  _read();
}

void nix_daemon_protocol_t::store_path_ca_map_t::_read() {
  m_num_entries = m__io->read_u8le();
  m_entries = std::unique_ptr<std::vector<std::unique_ptr<store_path_ca_entry_t>>>(
      new std::vector<std::unique_ptr<store_path_ca_entry_t>>());
  const int l_entries = num_entries();
  for (int i = 0; i < l_entries; i++) {
    m_entries->push_back(std::move(
        std::unique_ptr<store_path_ca_entry_t>(new store_path_ca_entry_t(m__io, this, m__root))));
  }
}

nix_daemon_protocol_t::store_path_ca_map_t::~store_path_ca_map_t() {
  _clean_up();
}

void nix_daemon_protocol_t::store_path_ca_map_t::_clean_up() {}

nix_daemon_protocol_t::query_substitutable_path_infos_request_t::
    query_substitutable_path_infos_request_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent,
                                             nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  m_paths = nullptr;
  m_paths_with_ca = nullptr;
  _read();
}

void nix_daemon_protocol_t::query_substitutable_path_infos_request_t::_read() {
  n_paths = true;
  if (_root()->protocol_version() < 22) {
    n_paths = false;
    m_paths = std::unique_ptr<store_path_set_t>(new store_path_set_t(m__io, this, m__root));
  }
  n_paths_with_ca = true;
  if (_root()->protocol_version() >= 22) {
    n_paths_with_ca = false;
    m_paths_with_ca =
        std::unique_ptr<store_path_ca_map_t>(new store_path_ca_map_t(m__io, this, m__root));
  }
}

nix_daemon_protocol_t::query_substitutable_path_infos_request_t::
    ~query_substitutable_path_infos_request_t() {
  _clean_up();
}

void nix_daemon_protocol_t::query_substitutable_path_infos_request_t::_clean_up() {
  if (!n_paths) {
  }
  if (!n_paths_with_ca) {
  }
}

nix_daemon_protocol_t::add_to_store_nar_request_t::add_to_store_nar_request_t(
    kaitai::kstream* p__io, kaitai::kstruct* p__parent, nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  m_path = nullptr;
  m_deriver = nullptr;
  m_nar_hash = nullptr;
  m_refs = nullptr;
  m_sigs = nullptr;
  m_ca = nullptr;
  _read();
}

void nix_daemon_protocol_t::add_to_store_nar_request_t::_read() {
  m_path = std::unique_ptr<store_path_t>(new store_path_t(m__io, this, m__root));
  m_deriver =
      std::unique_ptr<optional_store_path_t>(new optional_store_path_t(m__io, this, m__root));
  m_nar_hash = std::unique_ptr<nix_string_t>(new nix_string_t(m__io, this, m__root));
  m_refs = std::unique_ptr<store_path_set_t>(new store_path_set_t(m__io, this, m__root));
  m_registration_time = m__io->read_u8le();
  m_nar_size = m__io->read_u8le();
  m_ultimate = m__io->read_u8le();
  m_sigs = std::unique_ptr<nix_string_set_t>(new nix_string_set_t(m__io, this, m__root));
  m_ca = std::unique_ptr<nix_string_t>(new nix_string_t(m__io, this, m__root));
  m_repair = m__io->read_u8le();
  m_dont_check_sigs = m__io->read_u8le();
}

nix_daemon_protocol_t::add_to_store_nar_request_t::~add_to_store_nar_request_t() {
  _clean_up();
}

void nix_daemon_protocol_t::add_to_store_nar_request_t::_clean_up() {}

nix_daemon_protocol_t::register_drv_output_request_t::register_drv_output_request_t(
    kaitai::kstream* p__io, kaitai::kstruct* p__parent, nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  m_output_id = nullptr;
  m_output_path = nullptr;
  m_realisation = nullptr;
  _read();
}

void nix_daemon_protocol_t::register_drv_output_request_t::_read() {
  n_output_id = true;
  if (_root()->protocol_version() < 31) {
    n_output_id = false;
    m_output_id = std::unique_ptr<drv_output_t>(new drv_output_t(m__io, this, m__root));
  }
  n_output_path = true;
  if (_root()->protocol_version() < 31) {
    n_output_path = false;
    m_output_path = std::unique_ptr<nix_string_t>(new nix_string_t(m__io, this, m__root));
  }
  n_realisation = true;
  if (_root()->protocol_version() >= 31) {
    n_realisation = false;
    m_realisation = std::unique_ptr<realisation_t>(new realisation_t(m__io, this, m__root));
  }
}

nix_daemon_protocol_t::register_drv_output_request_t::~register_drv_output_request_t() {
  _clean_up();
}

void nix_daemon_protocol_t::register_drv_output_request_t::_clean_up() {
  if (!n_output_id) {
  }
  if (!n_output_path) {
  }
  if (!n_realisation) {
  }
}

nix_daemon_protocol_t::add_indirect_root_request_t::add_indirect_root_request_t(
    kaitai::kstream* p__io, kaitai::kstruct* p__parent, nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  m_path = nullptr;
  _read();
}

void nix_daemon_protocol_t::add_indirect_root_request_t::_read() {
  m_path = std::unique_ptr<nix_string_t>(new nix_string_t(m__io, this, m__root));
}

nix_daemon_protocol_t::add_indirect_root_request_t::~add_indirect_root_request_t() {
  _clean_up();
}

void nix_daemon_protocol_t::add_indirect_root_request_t::_clean_up() {}

nix_daemon_protocol_t::drv_outputs_t::drv_outputs_t(kaitai::kstream* p__io,
                                                    kaitai::kstruct* p__parent,
                                                    nix_daemon_protocol_t* p__root)
    : kaitai::kstruct(p__io) {
  m__parent = p__parent;
  m__root = p__root;
  m_outputs = nullptr;
  _read();
}

void nix_daemon_protocol_t::drv_outputs_t::_read() {
  m_num_outputs = m__io->read_u8le();
  m_outputs = std::unique_ptr<std::vector<std::unique_ptr<drv_output_entry_t>>>(
      new std::vector<std::unique_ptr<drv_output_entry_t>>());
  const int l_outputs = num_outputs();
  for (int i = 0; i < l_outputs; i++) {
    m_outputs->push_back(std::move(
        std::unique_ptr<drv_output_entry_t>(new drv_output_entry_t(m__io, this, m__root))));
  }
}

nix_daemon_protocol_t::drv_outputs_t::~drv_outputs_t() {
  _clean_up();
}

void nix_daemon_protocol_t::drv_outputs_t::_clean_up() {}
