#pragma once

// This is a generated file! Please edit source .ksy file and use kaitai-struct-compiler to rebuild

#include "kaitai/kaitaistruct.h"
#include <stdint.h>
#include <memory>
#include <vector>

#if KAITAI_STRUCT_VERSION < 9000L
#error "Incompatible Kaitai Struct C++/STL API: version 0.9 or later is required"
#endif

/**
 * The Nix daemon protocol ("worker protocol") is used for communication
 * between nix CLI tools and nix-daemon over unix sockets or SSH.
 * 
 * Wire format primitives:
 * - Integers: little-endian u64
 * - Strings: u64 length + bytes + padding to 8-byte boundary
 * - Booleans: u64 (0 = false, nonzero = true)
 * - Lists: u64 count + elements
 * - Sets: same as lists (sorted)
 * - Optional<T>: empty string = None for string-like, tag byte for others
 * 
 * Protocol version: 1.38 (current), minimum 1.10
 */

class nix_daemon_protocol_t : public kaitai::kstruct {

public:
    class valid_path_info_t;
    class stderr_stop_activity_t;
    class add_to_store_request_t;
    class derivation_output_entry_t;
    class query_valid_paths_request_t;
    class realisation_t;
    class query_missing_response_t;
    class request_t;
    class drv_output_t;
    class add_build_log_request_t;
    class add_text_to_store_request_t;
    class find_roots_response_t;
    class collect_garbage_response_t;
    class add_temp_root_request_t;
    class setting_overrides_t;
    class query_substitutable_path_infos_response_t;
    class stderr_result_t;
    class setting_pair_t;
    class is_valid_path_response_t;
    class logger_fields_t;
    class optional_duration_t;
    class has_substitutes_request_t;
    class nar_from_path_request_t;
    class nix_string_set_t;
    class server_handshake_info_t;
    class add_signatures_request_t;
    class nix_error_t;
    class query_references_request_t;
    class is_valid_path_request_t;
    class store_path_t;
    class query_substitutable_paths_request_t;
    class client_handshake_continuation_t;
    class stderr_message_t;
    class stderr_start_activity_t;
    class store_path_set_t;
    class store_path_ca_entry_t;
    class nix_string_t;
    class build_paths_with_results_request_t;
    class error_trace_t;
    class set_options_request_t;
    class query_realisation_response_t;
    class query_substitutable_path_info_request_t;
    class query_derivation_output_map_request_t;
    class build_derivation_request_t;
    class stderr_next_t;
    class derivation_output_map_t;
    class empty_request_t;
    class add_perm_root_request_t;
    class query_substitutable_path_info_response_t;
    class query_path_from_hash_part_request_t;
    class derived_path_list_t;
    class derived_path_t;
    class query_missing_request_t;
    class query_valid_derivers_request_t;
    class unkeyed_valid_path_info_t;
    class feature_exchange_t;
    class nix_string_list_t;
    class error_trace_list_t;
    class query_referrers_request_t;
    class nix_bytes_t;
    class query_derivation_output_names_request_t;
    class verify_store_request_t;
    class server_hello_t;
    class query_path_info_response_t;
    class client_hello_t;
    class query_realisation_request_t;
    class stderr_error_t;
    class build_paths_with_results_response_t;
    class ensure_path_request_t;
    class root_entry_t;
    class logger_field_t;
    class stderr_write_t;
    class query_deriver_request_t;
    class query_derivation_outputs_request_t;
    class query_realisation_response_old_t;
    class collect_garbage_request_t;
    class substitutable_path_info_entry_t;
    class query_path_info_request_t;
    class keyed_build_result_t;
    class drv_output_entry_t;
    class optional_store_path_t;
    class build_result_t;
    class stderr_read_t;
    class query_path_hash_request_t;
    class bool_response_t;
    class build_paths_request_t;
    class stderr_last_t;
    class add_multiple_to_store_request_t;
    class store_path_ca_map_t;
    class query_substitutable_path_infos_request_t;
    class add_to_store_nar_request_t;
    class register_drv_output_request_t;
    class add_indirect_root_request_t;
    class drv_outputs_t;

    enum gc_action_t {
        GC_ACTION_RETURN_LIVE = 0,
        GC_ACTION_RETURN_DEAD = 1,
        GC_ACTION_DELETE_DEAD = 2,
        GC_ACTION_DELETE_SPECIFIC = 3
    };

    enum logger_field_type_t {
        LOGGER_FIELD_TYPE_INT = 0,
        LOGGER_FIELD_TYPE_STRING = 1
    };

    enum build_status_t {
        BUILD_STATUS_BUILT = 0,
        BUILD_STATUS_SUBSTITUTED = 1,
        BUILD_STATUS_ALREADY_VALID = 2,
        BUILD_STATUS_PERMANENT_FAILURE = 3,
        BUILD_STATUS_INPUT_REJECTED = 4,
        BUILD_STATUS_OUTPUT_REJECTED = 5,
        BUILD_STATUS_TRANSIENT_FAILURE = 6,
        BUILD_STATUS_CACHED_FAILURE = 7,
        BUILD_STATUS_TIMED_OUT = 8,
        BUILD_STATUS_MISC_FAILURE = 9,
        BUILD_STATUS_DEPENDENCY_FAILED = 10,
        BUILD_STATUS_LOG_LIMIT_EXCEEDED = 11,
        BUILD_STATUS_NOT_DETERMINISTIC = 12,
        BUILD_STATUS_RESOLVED_DRV_FAILED = 13,
        BUILD_STATUS_NO_SUBSTITUTERS = 14
    };

    enum operation_t {
        OPERATION_IS_VALID_PATH = 1,
        OPERATION_HAS_SUBSTITUTES = 3,
        OPERATION_QUERY_PATH_HASH = 4,
        OPERATION_QUERY_REFERENCES = 5,
        OPERATION_QUERY_REFERRERS = 6,
        OPERATION_ADD_TO_STORE = 7,
        OPERATION_ADD_TEXT_TO_STORE = 8,
        OPERATION_BUILD_PATHS = 9,
        OPERATION_ENSURE_PATH = 10,
        OPERATION_ADD_TEMP_ROOT = 11,
        OPERATION_ADD_INDIRECT_ROOT = 12,
        OPERATION_SYNC_WITH_GC = 13,
        OPERATION_FIND_ROOTS = 14,
        OPERATION_QUERY_DERIVER = 18,
        OPERATION_SET_OPTIONS = 19,
        OPERATION_COLLECT_GARBAGE = 20,
        OPERATION_QUERY_SUBSTITUTABLE_PATH_INFO = 21,
        OPERATION_QUERY_DERIVATION_OUTPUTS = 22,
        OPERATION_QUERY_ALL_VALID_PATHS = 23,
        OPERATION_QUERY_FAILED_PATHS = 24,
        OPERATION_CLEAR_FAILED_PATHS = 25,
        OPERATION_QUERY_PATH_INFO = 26,
        OPERATION_QUERY_DERIVATION_OUTPUT_NAMES = 28,
        OPERATION_QUERY_PATH_FROM_HASH_PART = 29,
        OPERATION_QUERY_SUBSTITUTABLE_PATH_INFOS = 30,
        OPERATION_QUERY_VALID_PATHS = 31,
        OPERATION_QUERY_SUBSTITUTABLE_PATHS = 32,
        OPERATION_QUERY_VALID_DERIVERS = 33,
        OPERATION_OPTIMISE_STORE = 34,
        OPERATION_VERIFY_STORE = 35,
        OPERATION_BUILD_DERIVATION = 36,
        OPERATION_ADD_SIGNATURES = 37,
        OPERATION_NAR_FROM_PATH = 38,
        OPERATION_ADD_TO_STORE_NAR = 39,
        OPERATION_QUERY_MISSING = 40,
        OPERATION_QUERY_DERIVATION_OUTPUT_MAP = 41,
        OPERATION_REGISTER_DRV_OUTPUT = 42,
        OPERATION_QUERY_REALISATION = 43,
        OPERATION_ADD_MULTIPLE_TO_STORE = 44,
        OPERATION_ADD_BUILD_LOG = 45,
        OPERATION_BUILD_PATHS_WITH_RESULTS = 46,
        OPERATION_ADD_PERM_ROOT = 47,
        OPERATION_QUERY_ACTIVE_BUILDS = 48
    };

    enum trust_level_t {
        TRUST_LEVEL_UNKNOWN = 0,
        TRUST_LEVEL_TRUSTED = 1,
        TRUST_LEVEL_NOT_TRUSTED = 2
    };

    enum build_mode_t {
        BUILD_MODE_NORMAL = 0,
        BUILD_MODE_REPAIR = 1,
        BUILD_MODE_CHECK = 2
    };

    nix_daemon_protocol_t(uint16_t p_protocol_version, kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

private:
    void _read();
    void _clean_up();

public:
    ~nix_daemon_protocol_t();

    class valid_path_info_t : public kaitai::kstruct {

    public:

        valid_path_info_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~valid_path_info_t();

    private:
        std::unique_ptr<store_path_t> m_path;
        std::unique_ptr<unkeyed_valid_path_info_t> m_info;
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:
        store_path_t* path() const { return m_path.get(); }
        unkeyed_valid_path_info_t* info() const { return m_info.get(); }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    class stderr_stop_activity_t : public kaitai::kstruct {

    public:

        stderr_stop_activity_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~stderr_stop_activity_t();

    private:
        uint64_t m_act_id;
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:
        uint64_t act_id() const { return m_act_id; }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    /**
     * Add content to store. Protocol >= 1.25 uses new format.
     * NAR data follows via framed source after request.
     */

    class add_to_store_request_t : public kaitai::kstruct {

    public:

        add_to_store_request_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~add_to_store_request_t();

    private:
        std::unique_ptr<nix_string_t> m_name;
        bool n_name;

    public:
        bool _is_null_name() { name(); return n_name; };

    private:
        std::unique_ptr<nix_string_t> m_ca_method;
        bool n_ca_method;

    public:
        bool _is_null_ca_method() { ca_method(); return n_ca_method; };

    private:
        std::unique_ptr<store_path_set_t> m_refs;
        bool n_refs;

    public:
        bool _is_null_refs() { refs(); return n_refs; };

    private:
        uint64_t m_repair;
        bool n_repair;

    public:
        bool _is_null_repair() { repair(); return n_repair; };

    private:
        std::unique_ptr<nix_string_t> m_base_name;
        bool n_base_name;

    public:
        bool _is_null_base_name() { base_name(); return n_base_name; };

    private:
        uint64_t m_fixed;
        bool n_fixed;

    public:
        bool _is_null_fixed() { fixed(); return n_fixed; };

    private:
        uint8_t m_recursive;
        bool n_recursive;

    public:
        bool _is_null_recursive() { recursive(); return n_recursive; };

    private:
        std::unique_ptr<nix_string_t> m_hash_algo;
        bool n_hash_algo;

    public:
        bool _is_null_hash_algo() { hash_algo(); return n_hash_algo; };

    private:
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:
        nix_string_t* name() const { return m_name.get(); }

        /**
         * Content address method string (e.g. "nar:sha256")
         */
        nix_string_t* ca_method() const { return m_ca_method.get(); }
        store_path_set_t* refs() const { return m_refs.get(); }
        uint64_t repair() const { return m_repair; }
        nix_string_t* base_name() const { return m_base_name.get(); }
        uint64_t fixed() const { return m_fixed; }
        uint8_t recursive() const { return m_recursive; }
        nix_string_t* hash_algo() const { return m_hash_algo.get(); }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    class derivation_output_entry_t : public kaitai::kstruct {

    public:

        derivation_output_entry_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~derivation_output_entry_t();

    private:
        std::unique_ptr<nix_string_t> m_name;
        std::unique_ptr<optional_store_path_t> m_path;
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:
        nix_string_t* name() const { return m_name.get(); }
        optional_store_path_t* path() const { return m_path.get(); }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    class query_valid_paths_request_t : public kaitai::kstruct {

    public:

        query_valid_paths_request_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~query_valid_paths_request_t();

    private:
        std::unique_ptr<store_path_set_t> m_paths;
        uint64_t m_substitute;
        bool n_substitute;

    public:
        bool _is_null_substitute() { substitute(); return n_substitute; };

    private:
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:
        store_path_set_t* paths() const { return m_paths.get(); }

        /**
         * Boolean - whether to substitute missing paths
         */
        uint64_t substitute() const { return m_substitute; }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    class realisation_t : public kaitai::kstruct {

    public:

        realisation_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~realisation_t();

    private:
        std::unique_ptr<drv_output_t> m_id;
        std::unique_ptr<store_path_t> m_out_path;
        std::unique_ptr<nix_string_set_t> m_signatures;
        std::unique_ptr<drv_outputs_t> m_dependent_realisations;
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:
        drv_output_t* id() const { return m_id.get(); }
        store_path_t* out_path() const { return m_out_path.get(); }
        nix_string_set_t* signatures() const { return m_signatures.get(); }
        drv_outputs_t* dependent_realisations() const { return m_dependent_realisations.get(); }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    class query_missing_response_t : public kaitai::kstruct {

    public:

        query_missing_response_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~query_missing_response_t();

    private:
        std::unique_ptr<store_path_set_t> m_will_build;
        std::unique_ptr<store_path_set_t> m_will_substitute;
        std::unique_ptr<store_path_set_t> m_unknown;
        uint64_t m_download_size;
        uint64_t m_nar_size;
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:
        store_path_set_t* will_build() const { return m_will_build.get(); }
        store_path_set_t* will_substitute() const { return m_will_substitute.get(); }
        store_path_set_t* unknown() const { return m_unknown.get(); }
        uint64_t download_size() const { return m_download_size; }
        uint64_t nar_size() const { return m_nar_size; }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    /**
     * Client request to daemon
     */

    class request_t : public kaitai::kstruct {

    public:

        request_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~request_t();

    private:
        operation_t m_op;
        std::unique_ptr<kaitai::kstruct> m_payload;
        bool n_payload;

    public:
        bool _is_null_payload() { payload(); return n_payload; };

    private:
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:
        operation_t op() const { return m_op; }
        kaitai::kstruct* payload() const { return m_payload.get(); }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    /**
     * Derivation output identifier (drv path + output name)
     */

    class drv_output_t : public kaitai::kstruct {

    public:

        drv_output_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~drv_output_t();

    private:
        std::unique_ptr<nix_string_t> m_drv_path;
        std::unique_ptr<nix_string_t> m_output_name;
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:
        nix_string_t* drv_path() const { return m_drv_path.get(); }
        nix_string_t* output_name() const { return m_output_name.get(); }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    class add_build_log_request_t : public kaitai::kstruct {

    public:

        add_build_log_request_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~add_build_log_request_t();

    private:
        std::unique_ptr<nix_string_t> m_drv_path;
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:

        /**
         * StorePath as string
         */
        nix_string_t* drv_path() const { return m_drv_path.get(); }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    /**
     * Obsolete since 1.25
     */

    class add_text_to_store_request_t : public kaitai::kstruct {

    public:

        add_text_to_store_request_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~add_text_to_store_request_t();

    private:
        std::unique_ptr<nix_string_t> m_suffix;
        std::unique_ptr<nix_string_t> m_text;
        std::unique_ptr<store_path_set_t> m_refs;
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:
        nix_string_t* suffix() const { return m_suffix.get(); }
        nix_string_t* text() const { return m_text.get(); }
        store_path_set_t* refs() const { return m_refs.get(); }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    class find_roots_response_t : public kaitai::kstruct {

    public:

        find_roots_response_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~find_roots_response_t();

    private:
        uint64_t m_num_roots;
        std::unique_ptr<std::vector<std::unique_ptr<root_entry_t>>> m_roots;
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:
        uint64_t num_roots() const { return m_num_roots; }
        std::vector<std::unique_ptr<root_entry_t>>* roots() const { return m_roots.get(); }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    class collect_garbage_response_t : public kaitai::kstruct {

    public:

        collect_garbage_response_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~collect_garbage_response_t();

    private:
        std::unique_ptr<nix_string_set_t> m_paths;
        uint64_t m_bytes_freed;
        uint64_t m_obsolete;
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:
        nix_string_set_t* paths() const { return m_paths.get(); }
        uint64_t bytes_freed() const { return m_bytes_freed; }
        uint64_t obsolete() const { return m_obsolete; }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    class add_temp_root_request_t : public kaitai::kstruct {

    public:

        add_temp_root_request_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~add_temp_root_request_t();

    private:
        std::unique_ptr<store_path_t> m_path;
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:
        store_path_t* path() const { return m_path.get(); }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    class setting_overrides_t : public kaitai::kstruct {

    public:

        setting_overrides_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~setting_overrides_t();

    private:
        uint64_t m_num_settings;
        std::unique_ptr<std::vector<std::unique_ptr<setting_pair_t>>> m_settings;
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:
        uint64_t num_settings() const { return m_num_settings; }
        std::vector<std::unique_ptr<setting_pair_t>>* settings() const { return m_settings.get(); }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    class query_substitutable_path_infos_response_t : public kaitai::kstruct {

    public:

        query_substitutable_path_infos_response_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~query_substitutable_path_infos_response_t();

    private:
        uint64_t m_num_infos;
        std::unique_ptr<std::vector<std::unique_ptr<substitutable_path_info_entry_t>>> m_infos;
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:
        uint64_t num_infos() const { return m_num_infos; }
        std::vector<std::unique_ptr<substitutable_path_info_entry_t>>* infos() const { return m_infos.get(); }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    class stderr_result_t : public kaitai::kstruct {

    public:

        stderr_result_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~stderr_result_t();

    private:
        uint64_t m_act_id;
        uint64_t m_result_type;
        std::unique_ptr<logger_fields_t> m_fields;
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:
        uint64_t act_id() const { return m_act_id; }
        uint64_t result_type() const { return m_result_type; }
        logger_fields_t* fields() const { return m_fields.get(); }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    class setting_pair_t : public kaitai::kstruct {

    public:

        setting_pair_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~setting_pair_t();

    private:
        std::unique_ptr<nix_string_t> m_name;
        std::unique_ptr<nix_string_t> m_value;
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:
        nix_string_t* name() const { return m_name.get(); }
        nix_string_t* value() const { return m_value.get(); }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    class is_valid_path_response_t : public kaitai::kstruct {

    public:

        is_valid_path_response_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~is_valid_path_response_t();

    private:
        uint64_t m_valid;
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:
        uint64_t valid() const { return m_valid; }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    class logger_fields_t : public kaitai::kstruct {

    public:

        logger_fields_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~logger_fields_t();

    private:
        uint64_t m_num_fields;
        std::unique_ptr<std::vector<std::unique_ptr<logger_field_t>>> m_fields;
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:
        uint64_t num_fields() const { return m_num_fields; }
        std::vector<std::unique_ptr<logger_field_t>>* fields() const { return m_fields.get(); }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    /**
     * Optional microseconds duration
     */

    class optional_duration_t : public kaitai::kstruct {

    public:

        optional_duration_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~optional_duration_t();

    private:
        uint8_t m_tag;
        int64_t m_microseconds;
        bool n_microseconds;

    public:
        bool _is_null_microseconds() { microseconds(); return n_microseconds; };

    private:
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:
        uint8_t tag() const { return m_tag; }
        int64_t microseconds() const { return m_microseconds; }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    class has_substitutes_request_t : public kaitai::kstruct {

    public:

        has_substitutes_request_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~has_substitutes_request_t();

    private:
        std::unique_ptr<store_path_t> m_path;
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:
        store_path_t* path() const { return m_path.get(); }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    class nar_from_path_request_t : public kaitai::kstruct {

    public:

        nar_from_path_request_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~nar_from_path_request_t();

    private:
        std::unique_ptr<store_path_t> m_path;
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:
        store_path_t* path() const { return m_path.get(); }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    /**
     * Sorted set of strings (same wire format as list)
     */

    class nix_string_set_t : public kaitai::kstruct {

    public:

        nix_string_set_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~nix_string_set_t();

    private:
        uint64_t m_num_items;
        std::unique_ptr<std::vector<std::unique_ptr<nix_string_t>>> m_items;
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:
        uint64_t num_items() const { return m_num_items; }
        std::vector<std::unique_ptr<nix_string_t>>* items() const { return m_items.get(); }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    /**
     * Server info sent after version negotiation
     */

    class server_handshake_info_t : public kaitai::kstruct {

    public:

        server_handshake_info_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~server_handshake_info_t();

    private:
        std::unique_ptr<nix_string_t> m_daemon_version;
        bool n_daemon_version;

    public:
        bool _is_null_daemon_version() { daemon_version(); return n_daemon_version; };

    private:
        trust_level_t m_trust_level;
        bool n_trust_level;

    public:
        bool _is_null_trust_level() { trust_level(); return n_trust_level; };

    private:
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:
        nix_string_t* daemon_version() const { return m_daemon_version.get(); }
        trust_level_t trust_level() const { return m_trust_level; }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    class add_signatures_request_t : public kaitai::kstruct {

    public:

        add_signatures_request_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~add_signatures_request_t();

    private:
        std::unique_ptr<store_path_t> m_path;
        std::unique_ptr<nix_string_set_t> m_sigs;
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:
        store_path_t* path() const { return m_path.get(); }
        nix_string_set_t* sigs() const { return m_sigs.get(); }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    /**
     * Structured error (protocol >= 1.26)
     */

    class nix_error_t : public kaitai::kstruct {

    public:

        nix_error_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~nix_error_t();

    private:
        std::unique_ptr<nix_string_t> m_type;
        uint64_t m_level;
        std::unique_ptr<nix_string_t> m_name;
        std::unique_ptr<nix_string_t> m_msg;
        uint64_t m_have_pos;
        std::unique_ptr<error_trace_list_t> m_traces;
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:
        nix_string_t* type() const { return m_type.get(); }
        uint64_t level() const { return m_level; }
        nix_string_t* name() const { return m_name.get(); }
        nix_string_t* msg() const { return m_msg.get(); }
        uint64_t have_pos() const { return m_have_pos; }
        error_trace_list_t* traces() const { return m_traces.get(); }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    class query_references_request_t : public kaitai::kstruct {

    public:

        query_references_request_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~query_references_request_t();

    private:
        std::unique_ptr<store_path_t> m_path;
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:
        store_path_t* path() const { return m_path.get(); }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    class is_valid_path_request_t : public kaitai::kstruct {

    public:

        is_valid_path_request_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~is_valid_path_request_t();

    private:
        std::unique_ptr<store_path_t> m_path;
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:
        store_path_t* path() const { return m_path.get(); }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    /**
     * Store path, e.g. "/nix/store/abc123-hello-1.0"
     */

    class store_path_t : public kaitai::kstruct {

    public:

        store_path_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~store_path_t();

    private:
        std::unique_ptr<nix_string_t> m_path;
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:
        nix_string_t* path() const { return m_path.get(); }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    class query_substitutable_paths_request_t : public kaitai::kstruct {

    public:

        query_substitutable_paths_request_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~query_substitutable_paths_request_t();

    private:
        std::unique_ptr<store_path_set_t> m_paths;
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:
        store_path_set_t* paths() const { return m_paths.get(); }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    /**
     * After version exchange, client sends additional handshake info.
     * Fields depend on negotiated version.
     */

    class client_handshake_continuation_t : public kaitai::kstruct {

    public:

        client_handshake_continuation_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~client_handshake_continuation_t();

    private:
        uint64_t m_cpu_affinity_tag;
        bool n_cpu_affinity_tag;

    public:
        bool _is_null_cpu_affinity_tag() { cpu_affinity_tag(); return n_cpu_affinity_tag; };

    private:
        uint64_t m_cpu_affinity;
        bool n_cpu_affinity;

    public:
        bool _is_null_cpu_affinity() { cpu_affinity(); return n_cpu_affinity; };

    private:
        uint64_t m_reserve_space;
        bool n_reserve_space;

    public:
        bool _is_null_reserve_space() { reserve_space(); return n_reserve_space; };

    private:
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:
        uint64_t cpu_affinity_tag() const { return m_cpu_affinity_tag; }
        uint64_t cpu_affinity() const { return m_cpu_affinity; }
        uint64_t reserve_space() const { return m_reserve_space; }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    /**
     * During request processing, daemon sends stderr messages.
     * Client must handle these until receiving STDERR_LAST.
     */

    class stderr_message_t : public kaitai::kstruct {

    public:

        stderr_message_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~stderr_message_t();

    private:
        uint64_t m_msg_type;
        std::unique_ptr<kaitai::kstruct> m_payload;
        bool n_payload;

    public:
        bool _is_null_payload() { payload(); return n_payload; };

    private:
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:
        uint64_t msg_type() const { return m_msg_type; }
        kaitai::kstruct* payload() const { return m_payload.get(); }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    class stderr_start_activity_t : public kaitai::kstruct {

    public:

        stderr_start_activity_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~stderr_start_activity_t();

    private:
        uint64_t m_act_id;
        uint64_t m_verbosity;
        uint64_t m_activity_type;
        std::unique_ptr<nix_string_t> m_msg;
        std::unique_ptr<logger_fields_t> m_fields;
        uint64_t m_parent;
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:
        uint64_t act_id() const { return m_act_id; }
        uint64_t verbosity() const { return m_verbosity; }
        uint64_t activity_type() const { return m_activity_type; }
        nix_string_t* msg() const { return m_msg.get(); }
        logger_fields_t* fields() const { return m_fields.get(); }
        uint64_t parent() const { return m_parent; }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    class store_path_set_t : public kaitai::kstruct {

    public:

        store_path_set_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~store_path_set_t();

    private:
        uint64_t m_num_paths;
        std::unique_ptr<std::vector<std::unique_ptr<store_path_t>>> m_paths;
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:
        uint64_t num_paths() const { return m_num_paths; }
        std::vector<std::unique_ptr<store_path_t>>* paths() const { return m_paths.get(); }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    class store_path_ca_entry_t : public kaitai::kstruct {

    public:

        store_path_ca_entry_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~store_path_ca_entry_t();

    private:
        std::unique_ptr<store_path_t> m_path;
        std::unique_ptr<nix_string_t> m_ca;
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:
        store_path_t* path() const { return m_path.get(); }

        /**
         * Content address string, empty = none
         */
        nix_string_t* ca() const { return m_ca.get(); }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    /**
     * Length-prefixed string, padded to 8-byte boundary
     */

    class nix_string_t : public kaitai::kstruct {

    public:

        nix_string_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~nix_string_t();

    private:
        uint64_t m_len;
        std::string m_data;
        std::string m_padding;
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:
        uint64_t len() const { return m_len; }
        std::string data() const { return m_data; }
        std::string padding() const { return m_padding; }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    class build_paths_with_results_request_t : public kaitai::kstruct {

    public:

        build_paths_with_results_request_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~build_paths_with_results_request_t();

    private:
        std::unique_ptr<derived_path_list_t> m_paths;
        build_mode_t m_build_mode;
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:
        derived_path_list_t* paths() const { return m_paths.get(); }
        build_mode_t build_mode() const { return m_build_mode; }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    class error_trace_t : public kaitai::kstruct {

    public:

        error_trace_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~error_trace_t();

    private:
        uint64_t m_have_pos;
        std::unique_ptr<nix_string_t> m_msg;
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:
        uint64_t have_pos() const { return m_have_pos; }
        nix_string_t* msg() const { return m_msg.get(); }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    class set_options_request_t : public kaitai::kstruct {

    public:

        set_options_request_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~set_options_request_t();

    private:
        uint64_t m_keep_failed;
        uint64_t m_keep_going;
        uint64_t m_try_fallback;
        uint64_t m_verbosity;
        uint64_t m_max_build_jobs;
        uint64_t m_max_silent_time;
        uint64_t m_obsolete_use_build_hook;
        uint64_t m_verbose_build;
        uint64_t m_obsolete_log_type;
        uint64_t m_obsolete_print_build_trace;
        uint64_t m_build_cores;
        uint64_t m_use_substitutes;
        std::unique_ptr<setting_overrides_t> m_overrides;
        bool n_overrides;

    public:
        bool _is_null_overrides() { overrides(); return n_overrides; };

    private:
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:
        uint64_t keep_failed() const { return m_keep_failed; }
        uint64_t keep_going() const { return m_keep_going; }
        uint64_t try_fallback() const { return m_try_fallback; }
        uint64_t verbosity() const { return m_verbosity; }
        uint64_t max_build_jobs() const { return m_max_build_jobs; }
        uint64_t max_silent_time() const { return m_max_silent_time; }
        uint64_t obsolete_use_build_hook() const { return m_obsolete_use_build_hook; }
        uint64_t verbose_build() const { return m_verbose_build; }
        uint64_t obsolete_log_type() const { return m_obsolete_log_type; }
        uint64_t obsolete_print_build_trace() const { return m_obsolete_print_build_trace; }
        uint64_t build_cores() const { return m_build_cores; }
        uint64_t use_substitutes() const { return m_use_substitutes; }
        setting_overrides_t* overrides() const { return m_overrides.get(); }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    /**
     * Protocol >= 1.31
     */

    class query_realisation_response_t : public kaitai::kstruct {

    public:

        query_realisation_response_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~query_realisation_response_t();

    private:
        uint64_t m_num_realisations;
        std::unique_ptr<std::vector<std::unique_ptr<realisation_t>>> m_realisations;
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:
        uint64_t num_realisations() const { return m_num_realisations; }
        std::vector<std::unique_ptr<realisation_t>>* realisations() const { return m_realisations.get(); }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    class query_substitutable_path_info_request_t : public kaitai::kstruct {

    public:

        query_substitutable_path_info_request_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~query_substitutable_path_info_request_t();

    private:
        std::unique_ptr<store_path_t> m_path;
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:
        store_path_t* path() const { return m_path.get(); }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    class query_derivation_output_map_request_t : public kaitai::kstruct {

    public:

        query_derivation_output_map_request_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~query_derivation_output_map_request_t();

    private:
        std::unique_ptr<store_path_t> m_path;
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:
        store_path_t* path() const { return m_path.get(); }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    /**
     * Build a derivation. The derivation is sent inline (not read from store).
     * Derivation format is ATerm-based, parsed separately.
     */

    class build_derivation_request_t : public kaitai::kstruct {

    public:

        build_derivation_request_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~build_derivation_request_t();

    private:
        std::unique_ptr<store_path_t> m_drv_path;
        std::unique_ptr<nix_bytes_t> m_derivation;
        build_mode_t m_build_mode;
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:
        store_path_t* drv_path() const { return m_drv_path.get(); }

        /**
         * Serialized BasicDerivation in ATerm format
         */
        nix_bytes_t* derivation() const { return m_derivation.get(); }
        build_mode_t build_mode() const { return m_build_mode; }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    /**
     * Log message from daemon
     */

    class stderr_next_t : public kaitai::kstruct {

    public:

        stderr_next_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~stderr_next_t();

    private:
        std::unique_ptr<nix_string_t> m_msg;
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:
        nix_string_t* msg() const { return m_msg.get(); }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    class derivation_output_map_t : public kaitai::kstruct {

    public:

        derivation_output_map_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~derivation_output_map_t();

    private:
        uint64_t m_num_outputs;
        std::unique_ptr<std::vector<std::unique_ptr<derivation_output_entry_t>>> m_outputs;
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:
        uint64_t num_outputs() const { return m_num_outputs; }
        std::vector<std::unique_ptr<derivation_output_entry_t>>* outputs() const { return m_outputs.get(); }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    /**
     * Operations with no request payload
     */

    class empty_request_t : public kaitai::kstruct {

    public:

        empty_request_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~empty_request_t();

    private:
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    class add_perm_root_request_t : public kaitai::kstruct {

    public:

        add_perm_root_request_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~add_perm_root_request_t();

    private:
        std::unique_ptr<store_path_t> m_store_path;
        std::unique_ptr<nix_string_t> m_gc_root;
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:
        store_path_t* store_path() const { return m_store_path.get(); }

        /**
         * Absolute filesystem path for the symlink
         */
        nix_string_t* gc_root() const { return m_gc_root.get(); }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    class query_substitutable_path_info_response_t : public kaitai::kstruct {

    public:

        query_substitutable_path_info_response_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~query_substitutable_path_info_response_t();

    private:
        uint64_t m_found;
        std::unique_ptr<optional_store_path_t> m_deriver;
        bool n_deriver;

    public:
        bool _is_null_deriver() { deriver(); return n_deriver; };

    private:
        std::unique_ptr<store_path_set_t> m_references;
        bool n_references;

    public:
        bool _is_null_references() { references(); return n_references; };

    private:
        uint64_t m_download_size;
        bool n_download_size;

    public:
        bool _is_null_download_size() { download_size(); return n_download_size; };

    private:
        uint64_t m_nar_size;
        bool n_nar_size;

    public:
        bool _is_null_nar_size() { nar_size(); return n_nar_size; };

    private:
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:
        uint64_t found() const { return m_found; }
        optional_store_path_t* deriver() const { return m_deriver.get(); }
        store_path_set_t* references() const { return m_references.get(); }
        uint64_t download_size() const { return m_download_size; }
        uint64_t nar_size() const { return m_nar_size; }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    class query_path_from_hash_part_request_t : public kaitai::kstruct {

    public:

        query_path_from_hash_part_request_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~query_path_from_hash_part_request_t();

    private:
        std::unique_ptr<nix_string_t> m_hash_part;
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:

        /**
         * First 32 chars of store path hash
         */
        nix_string_t* hash_part() const { return m_hash_part.get(); }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    class derived_path_list_t : public kaitai::kstruct {

    public:

        derived_path_list_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~derived_path_list_t();

    private:
        uint64_t m_num_paths;
        std::unique_ptr<std::vector<std::unique_ptr<derived_path_t>>> m_paths;
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:
        uint64_t num_paths() const { return m_num_paths; }
        std::vector<std::unique_ptr<derived_path_t>>* paths() const { return m_paths.get(); }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    /**
     * String representation of a derived path.
     * Format: "/nix/store/...-foo" or "/nix/store/...-foo.drv^out,dev"
     */

    class derived_path_t : public kaitai::kstruct {

    public:

        derived_path_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~derived_path_t();

    private:
        std::unique_ptr<nix_string_t> m_path;
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:
        nix_string_t* path() const { return m_path.get(); }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    class query_missing_request_t : public kaitai::kstruct {

    public:

        query_missing_request_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~query_missing_request_t();

    private:
        std::unique_ptr<derived_path_list_t> m_targets;
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:
        derived_path_list_t* targets() const { return m_targets.get(); }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    class query_valid_derivers_request_t : public kaitai::kstruct {

    public:

        query_valid_derivers_request_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~query_valid_derivers_request_t();

    private:
        std::unique_ptr<store_path_t> m_path;
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:
        store_path_t* path() const { return m_path.get(); }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    class unkeyed_valid_path_info_t : public kaitai::kstruct {

    public:

        unkeyed_valid_path_info_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~unkeyed_valid_path_info_t();

    private:
        std::unique_ptr<optional_store_path_t> m_deriver;
        std::unique_ptr<nix_string_t> m_nar_hash;
        std::unique_ptr<store_path_set_t> m_references;
        uint64_t m_registration_time;
        uint64_t m_nar_size;
        uint64_t m_ultimate;
        bool n_ultimate;

    public:
        bool _is_null_ultimate() { ultimate(); return n_ultimate; };

    private:
        std::unique_ptr<nix_string_set_t> m_sigs;
        bool n_sigs;

    public:
        bool _is_null_sigs() { sigs(); return n_sigs; };

    private:
        std::unique_ptr<nix_string_t> m_ca;
        bool n_ca;

    public:
        bool _is_null_ca() { ca(); return n_ca; };

    private:
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:
        optional_store_path_t* deriver() const { return m_deriver.get(); }

        /**
         * SHA256 hash in base16
         */
        nix_string_t* nar_hash() const { return m_nar_hash.get(); }
        store_path_set_t* references() const { return m_references.get(); }
        uint64_t registration_time() const { return m_registration_time; }
        uint64_t nar_size() const { return m_nar_size; }
        uint64_t ultimate() const { return m_ultimate; }
        nix_string_set_t* sigs() const { return m_sigs.get(); }

        /**
         * Content address (empty = none)
         */
        nix_string_t* ca() const { return m_ca.get(); }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    /**
     * Protocol >= 1.38 feature negotiation
     */

    class feature_exchange_t : public kaitai::kstruct {

    public:

        feature_exchange_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~feature_exchange_t();

    private:
        std::unique_ptr<nix_string_set_t> m_features;
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:
        nix_string_set_t* features() const { return m_features.get(); }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    class nix_string_list_t : public kaitai::kstruct {

    public:

        nix_string_list_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~nix_string_list_t();

    private:
        uint64_t m_num_items;
        std::unique_ptr<std::vector<std::unique_ptr<nix_string_t>>> m_items;
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:
        uint64_t num_items() const { return m_num_items; }
        std::vector<std::unique_ptr<nix_string_t>>* items() const { return m_items.get(); }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    class error_trace_list_t : public kaitai::kstruct {

    public:

        error_trace_list_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~error_trace_list_t();

    private:
        uint64_t m_num_traces;
        std::unique_ptr<std::vector<std::unique_ptr<error_trace_t>>> m_traces;
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:
        uint64_t num_traces() const { return m_num_traces; }
        std::vector<std::unique_ptr<error_trace_t>>* traces() const { return m_traces.get(); }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    class query_referrers_request_t : public kaitai::kstruct {

    public:

        query_referrers_request_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~query_referrers_request_t();

    private:
        std::unique_ptr<store_path_t> m_path;
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:
        store_path_t* path() const { return m_path.get(); }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    /**
     * Length-prefixed bytes, padded to 8-byte boundary
     */

    class nix_bytes_t : public kaitai::kstruct {

    public:

        nix_bytes_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~nix_bytes_t();

    private:
        uint64_t m_len;
        std::string m_data;
        std::string m_padding;
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:
        uint64_t len() const { return m_len; }
        std::string data() const { return m_data; }
        std::string padding() const { return m_padding; }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    class query_derivation_output_names_request_t : public kaitai::kstruct {

    public:

        query_derivation_output_names_request_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~query_derivation_output_names_request_t();

    private:
        std::unique_ptr<store_path_t> m_path;
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:
        store_path_t* path() const { return m_path.get(); }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    class verify_store_request_t : public kaitai::kstruct {

    public:

        verify_store_request_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~verify_store_request_t();

    private:
        uint64_t m_check_contents;
        uint64_t m_repair;
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:
        uint64_t check_contents() const { return m_check_contents; }
        uint64_t repair() const { return m_repair; }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    /**
     * Response from daemon after receiving client_hello
     */

    class server_hello_t : public kaitai::kstruct {

    public:

        server_hello_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~server_hello_t();

    private:
        std::string m_magic;
        std::string m_padding1;
        uint64_t m_server_version;
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:

        /**
         * WORKER_MAGIC_2
         */
        std::string magic() const { return m_magic; }
        std::string padding1() const { return m_padding1; }

        /**
         * Protocol version (major << 8 | minor)
         */
        uint64_t server_version() const { return m_server_version; }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    class query_path_info_response_t : public kaitai::kstruct {

    public:

        query_path_info_response_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~query_path_info_response_t();

    private:
        uint64_t m_valid;
        std::unique_ptr<unkeyed_valid_path_info_t> m_info;
        bool n_info;

    public:
        bool _is_null_info() { info(); return n_info; };

    private:
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:
        uint64_t valid() const { return m_valid; }
        unkeyed_valid_path_info_t* info() const { return m_info.get(); }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    /**
     * Initial message from client to daemon
     */

    class client_hello_t : public kaitai::kstruct {

    public:

        client_hello_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~client_hello_t();

    private:
        std::string m_magic;
        std::string m_padding1;
        uint64_t m_client_version;
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:

        /**
         * WORKER_MAGIC_1
         */
        std::string magic() const { return m_magic; }
        std::string padding1() const { return m_padding1; }

        /**
         * Protocol version (major << 8 | minor)
         */
        uint64_t client_version() const { return m_client_version; }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    class query_realisation_request_t : public kaitai::kstruct {

    public:

        query_realisation_request_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~query_realisation_request_t();

    private:
        std::unique_ptr<drv_output_t> m_output_id;
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:
        drv_output_t* output_id() const { return m_output_id.get(); }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    /**
     * Error during request processing
     */

    class stderr_error_t : public kaitai::kstruct {

    public:

        stderr_error_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~stderr_error_t();

    private:
        std::unique_ptr<nix_error_t> m_error;
        bool n_error;

    public:
        bool _is_null_error() { error(); return n_error; };

    private:
        std::unique_ptr<nix_string_t> m_error_msg;
        bool n_error_msg;

    public:
        bool _is_null_error_msg() { error_msg(); return n_error_msg; };

    private:
        uint64_t m_status;
        bool n_status;

    public:
        bool _is_null_status() { status(); return n_status; };

    private:
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:
        nix_error_t* error() const { return m_error.get(); }
        nix_string_t* error_msg() const { return m_error_msg.get(); }
        uint64_t status() const { return m_status; }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    class build_paths_with_results_response_t : public kaitai::kstruct {

    public:

        build_paths_with_results_response_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~build_paths_with_results_response_t();

    private:
        uint64_t m_num_results;
        std::unique_ptr<std::vector<std::unique_ptr<keyed_build_result_t>>> m_results;
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:
        uint64_t num_results() const { return m_num_results; }
        std::vector<std::unique_ptr<keyed_build_result_t>>* results() const { return m_results.get(); }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    class ensure_path_request_t : public kaitai::kstruct {

    public:

        ensure_path_request_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~ensure_path_request_t();

    private:
        std::unique_ptr<store_path_t> m_path;
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:
        store_path_t* path() const { return m_path.get(); }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    class root_entry_t : public kaitai::kstruct {

    public:

        root_entry_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~root_entry_t();

    private:
        std::unique_ptr<nix_string_t> m_link;
        std::unique_ptr<store_path_t> m_target;
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:
        nix_string_t* link() const { return m_link.get(); }
        store_path_t* target() const { return m_target.get(); }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    class logger_field_t : public kaitai::kstruct {

    public:

        logger_field_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~logger_field_t();

    private:
        logger_field_type_t m_field_type;
        uint64_t m_int_value;
        bool n_int_value;

    public:
        bool _is_null_int_value() { int_value(); return n_int_value; };

    private:
        std::unique_ptr<nix_string_t> m_string_value;
        bool n_string_value;

    public:
        bool _is_null_string_value() { string_value(); return n_string_value; };

    private:
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:
        logger_field_type_t field_type() const { return m_field_type; }
        uint64_t int_value() const { return m_int_value; }
        nix_string_t* string_value() const { return m_string_value.get(); }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    /**
     * Daemon sending data to client (for streaming downloads)
     */

    class stderr_write_t : public kaitai::kstruct {

    public:

        stderr_write_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~stderr_write_t();

    private:
        std::unique_ptr<nix_string_t> m_data;
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:
        nix_string_t* data() const { return m_data.get(); }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    class query_deriver_request_t : public kaitai::kstruct {

    public:

        query_deriver_request_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~query_deriver_request_t();

    private:
        std::unique_ptr<store_path_t> m_path;
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:
        store_path_t* path() const { return m_path.get(); }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    class query_derivation_outputs_request_t : public kaitai::kstruct {

    public:

        query_derivation_outputs_request_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~query_derivation_outputs_request_t();

    private:
        std::unique_ptr<store_path_t> m_path;
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:
        store_path_t* path() const { return m_path.get(); }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    /**
     * Protocol < 1.31
     */

    class query_realisation_response_old_t : public kaitai::kstruct {

    public:

        query_realisation_response_old_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~query_realisation_response_old_t();

    private:
        std::unique_ptr<store_path_set_t> m_out_paths;
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:
        store_path_set_t* out_paths() const { return m_out_paths.get(); }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    class collect_garbage_request_t : public kaitai::kstruct {

    public:

        collect_garbage_request_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~collect_garbage_request_t();

    private:
        gc_action_t m_action;
        std::unique_ptr<store_path_set_t> m_paths_to_delete;
        uint64_t m_ignore_liveness;
        uint64_t m_max_freed;
        uint64_t m_obsolete1;
        uint64_t m_obsolete2;
        uint64_t m_obsolete3;
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:
        gc_action_t action() const { return m_action; }
        store_path_set_t* paths_to_delete() const { return m_paths_to_delete.get(); }
        uint64_t ignore_liveness() const { return m_ignore_liveness; }
        uint64_t max_freed() const { return m_max_freed; }
        uint64_t obsolete1() const { return m_obsolete1; }
        uint64_t obsolete2() const { return m_obsolete2; }
        uint64_t obsolete3() const { return m_obsolete3; }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    class substitutable_path_info_entry_t : public kaitai::kstruct {

    public:

        substitutable_path_info_entry_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~substitutable_path_info_entry_t();

    private:
        std::unique_ptr<store_path_t> m_path;
        std::unique_ptr<optional_store_path_t> m_deriver;
        std::unique_ptr<store_path_set_t> m_references;
        uint64_t m_download_size;
        uint64_t m_nar_size;
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:
        store_path_t* path() const { return m_path.get(); }
        optional_store_path_t* deriver() const { return m_deriver.get(); }
        store_path_set_t* references() const { return m_references.get(); }
        uint64_t download_size() const { return m_download_size; }
        uint64_t nar_size() const { return m_nar_size; }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    class query_path_info_request_t : public kaitai::kstruct {

    public:

        query_path_info_request_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~query_path_info_request_t();

    private:
        std::unique_ptr<store_path_t> m_path;
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:
        store_path_t* path() const { return m_path.get(); }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    class keyed_build_result_t : public kaitai::kstruct {

    public:

        keyed_build_result_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~keyed_build_result_t();

    private:
        std::unique_ptr<derived_path_t> m_path;
        std::unique_ptr<build_result_t> m_result;
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:
        derived_path_t* path() const { return m_path.get(); }
        build_result_t* result() const { return m_result.get(); }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    class drv_output_entry_t : public kaitai::kstruct {

    public:

        drv_output_entry_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~drv_output_entry_t();

    private:
        std::unique_ptr<drv_output_t> m_output_id;
        std::unique_ptr<realisation_t> m_realisation;
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:
        drv_output_t* output_id() const { return m_output_id.get(); }
        realisation_t* realisation() const { return m_realisation.get(); }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    /**
     * Empty string means None
     */

    class optional_store_path_t : public kaitai::kstruct {

    public:

        optional_store_path_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~optional_store_path_t();

    private:
        bool f_is_present;
        bool m_is_present;

    public:
        bool is_present();

    private:
        std::unique_ptr<nix_string_t> m_path;
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:
        nix_string_t* path() const { return m_path.get(); }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    class build_result_t : public kaitai::kstruct {

    public:

        build_result_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~build_result_t();

    private:
        build_status_t m_status;
        std::unique_ptr<nix_string_t> m_error_msg;
        uint64_t m_times_built;
        bool n_times_built;

    public:
        bool _is_null_times_built() { times_built(); return n_times_built; };

    private:
        uint64_t m_is_non_deterministic;
        bool n_is_non_deterministic;

    public:
        bool _is_null_is_non_deterministic() { is_non_deterministic(); return n_is_non_deterministic; };

    private:
        uint64_t m_start_time;
        bool n_start_time;

    public:
        bool _is_null_start_time() { start_time(); return n_start_time; };

    private:
        uint64_t m_stop_time;
        bool n_stop_time;

    public:
        bool _is_null_stop_time() { stop_time(); return n_stop_time; };

    private:
        std::unique_ptr<optional_duration_t> m_cpu_user;
        bool n_cpu_user;

    public:
        bool _is_null_cpu_user() { cpu_user(); return n_cpu_user; };

    private:
        std::unique_ptr<optional_duration_t> m_cpu_system;
        bool n_cpu_system;

    public:
        bool _is_null_cpu_system() { cpu_system(); return n_cpu_system; };

    private:
        std::unique_ptr<drv_outputs_t> m_built_outputs;
        bool n_built_outputs;

    public:
        bool _is_null_built_outputs() { built_outputs(); return n_built_outputs; };

    private:
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:
        build_status_t status() const { return m_status; }
        nix_string_t* error_msg() const { return m_error_msg.get(); }
        uint64_t times_built() const { return m_times_built; }
        uint64_t is_non_deterministic() const { return m_is_non_deterministic; }
        uint64_t start_time() const { return m_start_time; }
        uint64_t stop_time() const { return m_stop_time; }
        optional_duration_t* cpu_user() const { return m_cpu_user.get(); }
        optional_duration_t* cpu_system() const { return m_cpu_system.get(); }
        drv_outputs_t* built_outputs() const { return m_built_outputs.get(); }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    /**
     * Daemon requesting data from client (for streaming uploads)
     */

    class stderr_read_t : public kaitai::kstruct {

    public:

        stderr_read_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~stderr_read_t();

    private:
        uint64_t m_len;
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:
        uint64_t len() const { return m_len; }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    class query_path_hash_request_t : public kaitai::kstruct {

    public:

        query_path_hash_request_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~query_path_hash_request_t();

    private:
        std::unique_ptr<store_path_t> m_path;
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:
        store_path_t* path() const { return m_path.get(); }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    class bool_response_t : public kaitai::kstruct {

    public:

        bool_response_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~bool_response_t();

    private:
        uint64_t m_value;
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:
        uint64_t value() const { return m_value; }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    class build_paths_request_t : public kaitai::kstruct {

    public:

        build_paths_request_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~build_paths_request_t();

    private:
        std::unique_ptr<derived_path_list_t> m_paths;
        build_mode_t m_build_mode;
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:
        derived_path_list_t* paths() const { return m_paths.get(); }
        build_mode_t build_mode() const { return m_build_mode; }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    /**
     * End of request processing (success)
     */

    class stderr_last_t : public kaitai::kstruct {

    public:

        stderr_last_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~stderr_last_t();

    private:
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    class add_multiple_to_store_request_t : public kaitai::kstruct {

    public:

        add_multiple_to_store_request_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~add_multiple_to_store_request_t();

    private:
        uint64_t m_repair;
        uint64_t m_dont_check_sigs;
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:
        uint64_t repair() const { return m_repair; }
        uint64_t dont_check_sigs() const { return m_dont_check_sigs; }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    /**
     * Map from StorePath to optional ContentAddress (protocol >= 1.22)
     */

    class store_path_ca_map_t : public kaitai::kstruct {

    public:

        store_path_ca_map_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~store_path_ca_map_t();

    private:
        uint64_t m_num_entries;
        std::unique_ptr<std::vector<std::unique_ptr<store_path_ca_entry_t>>> m_entries;
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:
        uint64_t num_entries() const { return m_num_entries; }
        std::vector<std::unique_ptr<store_path_ca_entry_t>>* entries() const { return m_entries.get(); }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    class query_substitutable_path_infos_request_t : public kaitai::kstruct {

    public:

        query_substitutable_path_infos_request_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~query_substitutable_path_infos_request_t();

    private:
        std::unique_ptr<store_path_set_t> m_paths;
        bool n_paths;

    public:
        bool _is_null_paths() { paths(); return n_paths; };

    private:
        std::unique_ptr<store_path_ca_map_t> m_paths_with_ca;
        bool n_paths_with_ca;

    public:
        bool _is_null_paths_with_ca() { paths_with_ca(); return n_paths_with_ca; };

    private:
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:
        store_path_set_t* paths() const { return m_paths.get(); }
        store_path_ca_map_t* paths_with_ca() const { return m_paths_with_ca.get(); }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    class add_to_store_nar_request_t : public kaitai::kstruct {

    public:

        add_to_store_nar_request_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~add_to_store_nar_request_t();

    private:
        std::unique_ptr<store_path_t> m_path;
        std::unique_ptr<optional_store_path_t> m_deriver;
        std::unique_ptr<nix_string_t> m_nar_hash;
        std::unique_ptr<store_path_set_t> m_refs;
        uint64_t m_registration_time;
        uint64_t m_nar_size;
        uint64_t m_ultimate;
        std::unique_ptr<nix_string_set_t> m_sigs;
        std::unique_ptr<nix_string_t> m_ca;
        uint64_t m_repair;
        uint64_t m_dont_check_sigs;
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:
        store_path_t* path() const { return m_path.get(); }
        optional_store_path_t* deriver() const { return m_deriver.get(); }

        /**
         * SHA256 in base16
         */
        nix_string_t* nar_hash() const { return m_nar_hash.get(); }
        store_path_set_t* refs() const { return m_refs.get(); }
        uint64_t registration_time() const { return m_registration_time; }
        uint64_t nar_size() const { return m_nar_size; }
        uint64_t ultimate() const { return m_ultimate; }
        nix_string_set_t* sigs() const { return m_sigs.get(); }

        /**
         * Content address, empty = none
         */
        nix_string_t* ca() const { return m_ca.get(); }
        uint64_t repair() const { return m_repair; }
        uint64_t dont_check_sigs() const { return m_dont_check_sigs; }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    class register_drv_output_request_t : public kaitai::kstruct {

    public:

        register_drv_output_request_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~register_drv_output_request_t();

    private:
        std::unique_ptr<drv_output_t> m_output_id;
        bool n_output_id;

    public:
        bool _is_null_output_id() { output_id(); return n_output_id; };

    private:
        std::unique_ptr<nix_string_t> m_output_path;
        bool n_output_path;

    public:
        bool _is_null_output_path() { output_path(); return n_output_path; };

    private:
        std::unique_ptr<realisation_t> m_realisation;
        bool n_realisation;

    public:
        bool _is_null_realisation() { realisation(); return n_realisation; };

    private:
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:
        drv_output_t* output_id() const { return m_output_id.get(); }
        nix_string_t* output_path() const { return m_output_path.get(); }
        realisation_t* realisation() const { return m_realisation.get(); }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    class add_indirect_root_request_t : public kaitai::kstruct {

    public:

        add_indirect_root_request_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~add_indirect_root_request_t();

    private:
        std::unique_ptr<nix_string_t> m_path;
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:

        /**
         * Absolute filesystem path (not store path)
         */
        nix_string_t* path() const { return m_path.get(); }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

    class drv_outputs_t : public kaitai::kstruct {

    public:

        drv_outputs_t(kaitai::kstream* p__io, kaitai::kstruct* p__parent = nullptr, nix_daemon_protocol_t* p__root = nullptr);

    private:
        void _read();
        void _clean_up();

    public:
        ~drv_outputs_t();

    private:
        uint64_t m_num_outputs;
        std::unique_ptr<std::vector<std::unique_ptr<drv_output_entry_t>>> m_outputs;
        nix_daemon_protocol_t* m__root;
        kaitai::kstruct* m__parent;

    public:
        uint64_t num_outputs() const { return m_num_outputs; }
        std::vector<std::unique_ptr<drv_output_entry_t>>* outputs() const { return m_outputs.get(); }
        nix_daemon_protocol_t* _root() const { return m__root; }
        kaitai::kstruct* _parent() const { return m__parent; }
    };

private:
    uint16_t m_protocol_version;
    nix_daemon_protocol_t* m__root;
    kaitai::kstruct* m__parent;

public:

    /**
     * Negotiated protocol minor version (e.g., 38 for 1.38)
     */
    uint16_t protocol_version() const { return m_protocol_version; }
    nix_daemon_protocol_t* _root() const { return m__root; }
    kaitai::kstruct* _parent() const { return m__parent; }
};
