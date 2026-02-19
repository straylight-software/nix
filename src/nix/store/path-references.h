#pragma once
///@file

#include <functional>
#include <vector>

#include "nix/store/path.h"
#include "nix/store/references.h"
#include "nix/util/source-accessor.h"

namespace nix {

StorePathSet scan_for_references(Sink& to_tee, const Path& path, const StorePathSet& refs);

class PathRefScanSink : public RefScanSink {
  std::map<std::string, StorePath> backMap;

  PathRefScanSink(string_set_t&& hashes, std::map<std::string, StorePath>&& backMap);

public:
  static PathRefScanSink fromPaths(const StorePathSet& refs);

  StorePathSet getResultPaths();
};

/**
 * Result of scanning a single file for references.
 */
struct FileRefScanResult {
  canon_path_t filePath;     ///< The file that was scanned
  StorePathSet found_refs; ///< Which store paths were found in this file
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
 * @param accessor Source accessor to read the tree
 * @param root_path Root path to scan
 * @param refs Set of store paths to search for
 * @param callback Called for each file that contains at least one reference
 */
void scan_for_references_deep(SourceAccessor& accessor, const canon_path_t& root_path,
                           const StorePathSet& refs,
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
 * @param accessor Source accessor to read the tree
 * @param root_path Root path to scan
 * @param refs Set of store paths to search for
 * @return Map from file paths to the set of references found in each file
 */
std::map<canon_path_t, StorePathSet> scan_for_references_deep(SourceAccessor& accessor,
                                                        const canon_path_t& root_path,
                                                        const StorePathSet& refs);

} // namespace nix
