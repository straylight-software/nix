#pragma once
///@file

#include <optional>
#include <string>
#include <string_view>

#include "nix/util/fs-sink.h"
#include "nix/util/hash.h"
#include "nix/util/serialise.h"
#include "nix/util/source-path.h"
#include "nix/util/types.h"

namespace nix::git {

enum struct object_type_t {
  blob,
  tree_t,
  // Commit,
  // Tag,
};

using raw_mode_t = uint32_t;

enum struct Mode : raw_mode_t {
  directory_t = 0040000,
  regular = 0100644,
  executable = 0100755,
  symlink = 0120000,
};

std::optional<Mode> decode_mode(raw_mode_t m);

/**
 * An anonymous git tree object entry (no name part).
 */
struct tree_entry {
  Mode mode;
  Hash hash;

  bool operator==(const tree_entry&) const = default;
  auto operator<=>(const tree_entry&) const = default;
};

/**
 * A git tree object, fully decoded and stored in memory.
 *
 * directory_t names must end in a `/` for sake of sorting. See
 * https://github.com/mirage/irmin/issues/352
 */
using tree_t = std::map<std::string, tree_entry>;

/**
 * Callback for processing a child hash with `parse`
 *
 * The function should
 *
 * 1. Obtain the file system objects denoted by `gitHash`
 *
 * 2. Ensure they match `mode`
 *
 * 3. Feed them into the same sink `parse` was called with
 *
 * Implementations may seek to memoize resources (bandwidth, storage,
 * etc.) for the same git hash.
 */
using sink_hook_t = void(const canon_path_t& name, tree_entry entry);

/**
 * Parse the "blob " or "tree " prefix.
 *
 * @throws if prefix not recognized
 */
object_type_t parse_object_type(
    source_t& source,
    const experimental_feature_settings_t& xp_settings = experimental_feature_settings);

/**
 * These 3 modes are represented by blob objects.
 *
 * Sometimes we need this information to disambiguate how a blob is
 * being used to better match our own "file system object" data model.
 */
enum struct blob_mode_t : raw_mode_t {
  regular = static_cast<raw_mode_t>(Mode::regular),
  executable = static_cast<raw_mode_t>(Mode::executable),
  symlink = static_cast<raw_mode_t>(Mode::symlink),
};

void parse_blob(file_system_object_sink_t& sink, const canon_path_t& sink_path, source_t& source,
                blob_mode_t blob_mode,
                const experimental_feature_settings_t& xp_settings = experimental_feature_settings);

/**
 * @param hash_algo must be `HashAlgo::SHA1` or `HashAlgo::SHA256` for now.
 */
void parse_tree(file_system_object_sink_t& sink, const canon_path_t& sink_path, source_t& source,
                hash_algorithm_t hash_algo, std::function<sink_hook_t> hook,
                const experimental_feature_settings_t& xp_settings = experimental_feature_settings);

/**
 * Helper putting the previous three `parse*` functions together.
 *
 * @param root_mode_if_blob How to interpret a root blob, for which there is no
 * disambiguating dir entry to answer that questino. If the root it not
 * a blob, this is ignored.
 *
 * @param hash_algo must be `HashAlgo::SHA1` or `HashAlgo::SHA256` for now.
 */
void parse(file_system_object_sink_t& sink, const canon_path_t& sink_path, source_t& source,
           blob_mode_t root_mode_if_blob, hash_algorithm_t hash_algo,
           std::function<sink_hook_t> hook,
           const experimental_feature_settings_t& xp_settings = experimental_feature_settings);

/**
 * Assists with writing a `sink_hook_t` step (2).
 */
std::optional<Mode> convert_mode(source_accessor_t::Type type);

/**
 * Simplified version of `sink_hook_t` for `restore`.
 *
 * Given a `Hash`, return a `source_accessor_t` and `canon_path_t` pointing to
 * the file system object with that path.
 */
using restore_hook_t = source_path_t(Hash);

/**
 * Wrapper around `parse` and `restore_sink_t`
 *
 * @param hash_algo must be `HashAlgo::SHA1` or `HashAlgo::SHA256` for now.
 */
void restore(file_system_object_sink_t& sink, source_t& source, hash_algorithm_t hash_algo,
             std::function<restore_hook_t> hook);

/**
 * Dumps a single file to a sink
 *
 * @param xp_settings for testing purposes
 */
void dump_blob_prefix(
    uint64_t size, sink_t& sink,
    const experimental_feature_settings_t& xp_settings = experimental_feature_settings);

/**
 * Dumps a representation of a git tree to a sink
 */
void dump_tree(const tree_t& entries, sink_t& sink,
               const experimental_feature_settings_t& xp_settings = experimental_feature_settings);

/**
 * Callback for processing a child with `dump`
 *
 * The function should return the git hash and mode of the file at the
 * given path in the accessor passed to `dump`.
 *
 * Note that if the child is a directory, its child in must also be so
 * processed in order to compute this information.
 */
using dump_hook_t = tree_entry(const source_path_t& path);

Mode dump(const source_path_t& path, sink_t& sink, std::function<dump_hook_t> hook,
          path_filter_t& filter = default_path_filter,
          const experimental_feature_settings_t& xp_settings = experimental_feature_settings);

/**
 * Recursively dumps path, hashing as we go.
 *
 * A smaller wrapper around `dump`.
 */
tree_entry dump_hash(hash_algorithm_t ha, const source_path_t& path,
                     path_filter_t& filter = default_path_filter);

/**
 * A line from the output of `git ls-remote --symref`.
 *
 * These can be of two kinds:
 *
 * - symbolic references of the form
 *
 *   ```
 *   ref: {target} {reference}
 *   ```
 *   where {target} is itself a reference and {reference} is optional
 *
 * - Object references of the form
 *
 *   ```
 *   {target}  {reference}
 *   ```
 *   where {target} is a commit id and {reference} is mandatory
 */
struct ls_remote_ref_line_t {
  enum struct Kind { symbolic, Object };
  Kind kind;
  std::string target;
  std::optional<std::string> reference;
};

/**
 * Parse an `ls_remote_ref_line_t`
 */
std::optional<ls_remote_ref_line_t> parse_ls_remote_line(std::string_view line);

} // namespace nix::git
