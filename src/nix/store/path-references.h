#pragma once
///@file

#include <functional>
#include <vector>

#include "nix/store/path.h"
#include "nix/store/references.h"
#include "nix/util/source-accessor.h"

namespace nix {

store_path_set_t scan_for_references(sink_t& to_tee, const Path& path,
                                     const store_path_set_t& refs);

class PathRefScanSink : public RefScanSink {
  std::map<std::string, store_path_t> backMap;

  PathRefScanSink(string_set_t&& hashes, std::map<std::string, store_path_t>&& backMap);

public:
  static PathRefScanSink fromPaths(const store_path_set_t& refs);

  store_path_set_t getResultPaths();
};

/**
 * Result of scanning a single file for references.
 */
struct FileRefScanResult {
  canon_path_t filePath;       ///< The file that was scanned
  store_path_set_t found_refs; ///< Which store paths were found in this file
};

/**
 * Scan a store path tree and report which references appear in which files.
 *
 * This is like scan_for_references() but provides per-file granularity.
 * Useful for cycle detection and detailed dependency analysis like `nix why-depends --precise`.
 *
 * The function walks the tree using the provided accessor and streams each file's
 * contents through a RefScanSink to detect hash references. For each file that
 * contains at least one reference, a callback is invoked with the file path and
 * the set of references found.
 *
 * Note: This function only searches for the hash part of store paths (e.g.,
 * "dc04vv14dak1c1r48qa0m23vr9jy8sm0"), not the name part. A store path like
 * "/nix/store/dc04vv14dak1c1r48qa0m23vr9jy8sm0-foo" will be detected if the
 * hash appears anywhere in the scanned content, regardless of the "-foo" suffix.
 *
 * @param accessor source_t accessor to read the tree
 * @param root_path Root path to scan
 * @param refs Set of store paths to search for
 * @param callback Called for each file that contains at least one reference
 */
void scan_for_references_deep(source_accessor_t& accessor, const canon_path_t& root_path,
                              const store_path_set_t& refs,
                              std::function<void(FileRefScanResult)> callback);

/**
 * Scan a store path tree and return which references appear in which files.
 *
 * This is a convenience wrapper around the callback-based scan_for_references_deep()
 * that collects all results into a map for efficient lookups.
 *
 * Note: This function only searches for the hash part of store paths, not the name part.
 * See the callback-based overload for details.
 *
 * @param accessor source_t accessor to read the tree
 * @param root_path Root path to scan
 * @param refs Set of store paths to search for
 * @return Map from file paths to the set of references found in each file
 */
std::map<canon_path_t, store_path_set_t> scan_for_references_deep(source_accessor_t& accessor,
                                                                  const canon_path_t& root_path,
                                                                  const store_path_set_t& refs);

} // namespace nix
