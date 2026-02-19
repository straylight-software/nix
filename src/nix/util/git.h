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
  Blob,
  tree_t,
  // Commit,
  // Tag,
};

using raw_mode_t = uint32_t;

enum struct Mode : raw_mode_t {
  directory_t = 0040000,
  Regular = 0100644,
  Executable = 0100755,
  Symlink = 0120000,
};

std::optional<Mode> decodeMode(raw_mode_t m);

/**
 * An anonymous Git tree object entry (no name part).
 */
struct TreeEntry {
  Mode mode;
  Hash hash;

  bool operator==(const TreeEntry&) const = default;
  auto operator<=>(const TreeEntry&) const = default;
};

/**
 * A Git tree object, fully decoded and stored in memory.
 *
 * directory_t names must end in a `/` for sake of sorting. See
 * https://github.com/mirage/irmin/issues/352
 */
using tree_t = std::map<std::string, TreeEntry>;

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
 * etc.) for the same Git hash.
 */
using sink_hook_t = void(const canon_path_t& name, TreeEntry entry);

/**
 * Parse the "blob " or "tree " prefix.
 *
 * @throws if prefix not recognized
 */
object_type_t
parseObjectType(Source& source,
                const experimental_feature_settings_t& xpSettings = experimentalFeatureSettings);

/**
 * These 3 modes are represented by blob objects.
 *
 * Sometimes we need this information to disambiguate how a blob is
 * being used to better match our own "file system object" data model.
 */
enum struct blob_mode_t : raw_mode_t {
  Regular = static_cast<raw_mode_t>(Mode::Regular),
  Executable = static_cast<raw_mode_t>(Mode::Executable),
  Symlink = static_cast<raw_mode_t>(Mode::Symlink),
};

void parseBlob(file_system_object_sink_t& sink, const canon_path_t& sinkPath, Source& source,
               blob_mode_t blobMode,
               const experimental_feature_settings_t& xpSettings = experimentalFeatureSettings);

/**
 * @param hashAlgo must be `HashAlgo::SHA1` or `HashAlgo::SHA256` for now.
 */
void parseTree(file_system_object_sink_t& sink, const canon_path_t& sinkPath, Source& source,
               hash_algorithm_t hashAlgo, std::function<sink_hook_t> hook,
               const experimental_feature_settings_t& xpSettings = experimentalFeatureSettings);

/**
 * Helper putting the previous three `parse*` functions together.
 *
 * @param rootModeIfBlob How to interpret a root blob, for which there is no
 * disambiguating dir entry to answer that questino. If the root it not
 * a blob, this is ignored.
 *
 * @param hashAlgo must be `HashAlgo::SHA1` or `HashAlgo::SHA256` for now.
 */
void parse(file_system_object_sink_t& sink, const canon_path_t& sinkPath, Source& source,
           blob_mode_t rootModeIfBlob, hash_algorithm_t hashAlgo, std::function<sink_hook_t> hook,
           const experimental_feature_settings_t& xpSettings = experimentalFeatureSettings);

/**
 * Assists with writing a `sink_hook_t` step (2).
 */
std::optional<Mode> convertMode(SourceAccessor::Type type);

/**
 * Simplified version of `sink_hook_t` for `restore`.
 *
 * Given a `Hash`, return a `SourceAccessor` and `canon_path_t` pointing to
 * the file system object with that path.
 */
using restore_hook_t = source_path_t(Hash);

/**
 * Wrapper around `parse` and `restore_sink_t`
 *
 * @param hashAlgo must be `HashAlgo::SHA1` or `HashAlgo::SHA256` for now.
 */
void restore(file_system_object_sink_t& sink, Source& source, hash_algorithm_t hashAlgo,
             std::function<restore_hook_t> hook);

/**
 * Dumps a single file to a sink
 *
 * @param xpSettings for testing purposes
 */
void dumpBlobPrefix(uint64_t size, Sink& sink,
                    const experimental_feature_settings_t& xpSettings = experimentalFeatureSettings);

/**
 * Dumps a representation of a git tree to a sink
 */
void dumpTree(const tree_t& entries, Sink& sink,
              const experimental_feature_settings_t& xpSettings = experimentalFeatureSettings);

/**
 * Callback for processing a child with `dump`
 *
 * The function should return the Git hash and mode of the file at the
 * given path in the accessor passed to `dump`.
 *
 * Note that if the child is a directory, its child in must also be so
 * processed in order to compute this information.
 */
using dump_hook_t = TreeEntry(const source_path_t& path);

Mode dump(const source_path_t& path, Sink& sink, std::function<dump_hook_t> hook,
          path_filter_t& filter = defaultPathFilter,
          const experimental_feature_settings_t& xpSettings = experimentalFeatureSettings);

/**
 * Recursively dumps path, hashing as we go.
 *
 * A smaller wrapper around `dump`.
 */
TreeEntry dumpHash(hash_algorithm_t ha, const source_path_t& path,
                   path_filter_t& filter = defaultPathFilter);

/**
 * A line from the output of `git ls-remote --symref`.
 *
 * These can be of two kinds:
 *
 * - Symbolic references of the form
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
  enum struct Kind { Symbolic, Object };
  Kind kind;
  std::string target;
  std::optional<std::string> reference;
};

/**
 * Parse an `ls_remote_ref_line_t`
 */
std::optional<ls_remote_ref_line_t> parseLsRemoteLine(std::string_view line);

} // namespace nix::git
