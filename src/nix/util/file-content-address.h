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
   * Flat-file. The contents of a single file exactly.
   *
   * See `file-system-object/content-address.md#serial-flat` in the
   * manual.
   */
  Flat,

  /**
   * Nix Archive. Serializes the file-system object in
   * Nix Archive format.
   *
   * See `file-system-object/content-address.md#serial-nix-archive` in
   * the manual.
   */
  NixArchive,
};

/**
 * Parse a `file_serialisation_method_t` by name. Choice of:
 *
 *  - `flat`: `file_serialisation_method_t::Flat`
 *  - `nar`: `file_serialisation_method_t::NixArchive`
 *
 * Opposite of `renderFileSerialisationMethod`.
 */
file_serialisation_method_t parseFileSerialisationMethod(std::string_view input);

/**
 * Render a `file_serialisation_method_t` by name.
 *
 * Opposite of `parseFileSerialisationMethod`.
 */
std::string_view renderFileSerialisationMethod(file_serialisation_method_t method);

/**
 * Dump a serialization of the given file system object.
 */
void dumpPath(const source_path_t& path, Sink& sink, file_serialisation_method_t method,
              path_filter_t& filter = defaultPathFilter);

/**
 * Restore a serialisation of the given file system object.
 *
 * \todo use an arbitrary `file_system_object_sink_t`.
 */
void restorePath(const Path& path, Source& source, file_serialisation_method_t method,
                 bool startFsync = false);

/**
 * Compute the hash of the given file system object according to the
 * given method.
 *
 * the hash is defined as (in pseudocode):
 *
 * ```
 * hashString(ha, dumpPath(...))
 * ```
 */
hash_result_t hashPath(const source_path_t& path, file_serialisation_method_t method, hash_algorithm_t ha,
                    path_filter_t& filter = defaultPathFilter);

/**
 * An enumeration of the ways we can ingest file system
 * objects, producing a hash or digest.
 *
 * See `file-system-object/content-address.md` in the manual for a
 * user-facing description of this concept.
 */
enum struct file_ingestion_method_t : uint8_t {
  /**
   * Hash `file_serialisation_method_t::Flat` serialisation.
   *
   * See `file-system-object/content-address.md#serial-flat` in the
   * manual.
   */
  Flat,

  /**
   * Hash `file_serialisation_method_t::NixArchive` serialisation.
   *
   * See `file-system-object/content-address.md#serial-flat` in the
   * manual.
   */
  NixArchive,

  /**
   * Git hashing.
   *
   * Part of `experimental_feature_t::GitHashing`.
   *
   * See `file-system-object/content-address.md#serial-git` in the
   * manual.
   */
  Git,
};

/**
 * Parse a `file_ingestion_method_t` by name. Choice of:
 *
 *  - `flat`: `file_ingestion_method_t::Flat`
 *  - `nar`: `file_ingestion_method_t::NixArchive`
 *  - `git`: `file_ingestion_method_t::Git`
 *
 * Opposite of `renderFileIngestionMethod`.
 */
file_ingestion_method_t parseFileIngestionMethod(std::string_view input);

/**
 * Render a `file_ingestion_method_t` by name.
 *
 * Opposite of `parseFileIngestionMethod`.
 */
std::string_view renderFileIngestionMethod(file_ingestion_method_t method);

/**
 * Compute the hash of the given file system object according to the
 * given method, and for some ingestion methods, the size of the
 * serialisation.
 *
 * Unlike the other `hashPath`, this works on an arbitrary
 * `file_ingestion_method_t` instead of `file_serialisation_method_t`, but
 * may not return the size as this is this is not a both simple and
 * useful defined for a merkle format.
 */
std::pair<Hash, std::optional<uint64_t>> hashPath(const source_path_t& path,
                                                  file_ingestion_method_t method, hash_algorithm_t ha,
                                                  path_filter_t& filter = defaultPathFilter);

} // namespace nix
