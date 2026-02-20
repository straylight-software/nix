#pragma once
///@file

#include "nix/util/source-accessor.h"

namespace nix {

struct source_path_t;

/**
 * An enumeration of the ways we can serialize file system
 * objects.
 *
 * See `file-system-object/content-address.md#serial` in the manual for
 * a user-facing description of this concept, but note that this type is also
 * used for storing or sending copies; not just for addressing.
 * Note also that there are other content addressing methods that don't
 * correspond to a serialisation method.
 */
enum struct file_serialisation_method_t : uint8_t {
  /**
   * flat-file. The contents of a single file exactly.
   *
   * See `file-system-object/content-address.md#serial-flat` in the
   * manual.
   */
  flat,

  /**
   * Nix Archive. Serializes the file-system object in
   * Nix Archive format.
   *
   * See `file-system-object/content-address.md#serial-nix-archive` in
   * the manual.
   */
  nix_archive,
};

/**
 * Parse a `file_serialisation_method_t` by name. Choice of:
 *
 *  - `flat`: `file_serialisation_method_t::flat`
 *  - `nar`: `file_serialisation_method_t::nix_archive`
 *
 * Opposite of `render_file_serialisation_method`.
 */
file_serialisation_method_t parse_file_serialisation_method(std::string_view input);

/**
 * Render a `file_serialisation_method_t` by name.
 *
 * Opposite of `parse_file_serialisation_method`.
 */
std::string_view render_file_serialisation_method(file_serialisation_method_t method);

/**
 * Dump a serialization of the given file system object.
 */
void dump_path(const source_path_t& path, sink_t& sink, file_serialisation_method_t method,
              path_filter_t& filter = default_path_filter);

/**
 * Restore a serialisation of the given file system object.
 *
 * \todo use an arbitrary `file_system_object_sink_t`.
 */
void restore_path(const Path& path, source_t& source, file_serialisation_method_t method,
                 bool start_fsync = false);

/**
 * Compute the hash of the given file system object according to the
 * given method.
 *
 * the hash is defined as (in pseudocode):
 *
 * ```
 * hash_string(ha, dump_path(...))
 * ```
 */
hash_result_t hash_path(const source_path_t& path, file_serialisation_method_t method, hash_algorithm_t ha,
                    path_filter_t& filter = default_path_filter);

/**
 * An enumeration of the ways we can ingest file system
 * objects, producing a hash or digest.
 *
 * See `file-system-object/content-address.md` in the manual for a
 * user-facing description of this concept.
 */
enum struct file_ingestion_method_t : uint8_t {
  /**
   * Hash `file_serialisation_method_t::flat` serialisation.
   *
   * See `file-system-object/content-address.md#serial-flat` in the
   * manual.
   */
  flat,

  /**
   * Hash `file_serialisation_method_t::nix_archive` serialisation.
   *
   * See `file-system-object/content-address.md#serial-flat` in the
   * manual.
   */
  nix_archive,

  /**
   * git hashing.
   *
   * Part of `experimental_feature_t::git_hashing`.
   *
   * See `file-system-object/content-address.md#serial-git` in the
   * manual.
   */
  git,
};

/**
 * Parse a `file_ingestion_method_t` by name. Choice of:
 *
 *  - `flat`: `file_ingestion_method_t::flat`
 *  - `nar`: `file_ingestion_method_t::nix_archive`
 *  - `git`: `file_ingestion_method_t::git`
 *
 * Opposite of `render_file_ingestion_method`.
 */
file_ingestion_method_t parse_file_ingestion_method(std::string_view input);

/**
 * Render a `file_ingestion_method_t` by name.
 *
 * Opposite of `parse_file_ingestion_method`.
 */
std::string_view render_file_ingestion_method(file_ingestion_method_t method);

/**
 * Compute the hash of the given file system object according to the
 * given method, and for some ingestion methods, the size of the
 * serialisation.
 *
 * Unlike the other `hash_path`, this works on an arbitrary
 * `file_ingestion_method_t` instead of `file_serialisation_method_t`, but
 * may not return the size as this is this is not a both simple and
 * useful defined for a merkle format.
 */
std::pair<Hash, std::optional<uint64_t>> hash_path(const source_path_t& path,
                                                  file_ingestion_method_t method, hash_algorithm_t ha,
                                                  path_filter_t& filter = default_path_filter);

} // namespace nix
