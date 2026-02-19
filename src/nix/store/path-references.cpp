#include "nix/store/path-references.h"

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <map>
#include <mutex>

#include "nix/util/archive.h"
#include "nix/util/canon-path.h"
#include "nix/util/hash.h"
#include "nix/util/logging.h"
#include "nix/util/source-accessor.h"

namespace nix {

PathRefScanSink::PathRefScanSink(string_set_t&& hashes, std::map<std::string, StorePath>&& backMap)
    : RefScanSink(std::move(hashes)), backMap(std::move(backMap)) {}

PathRefScanSink PathRefScanSink::fromPaths(const StorePathSet& refs) {
  string_set_t hashes;
  std::map<std::string, StorePath> backMap;

  for (auto& i : refs) {
    std::string hash_part(i.hash_part());
    auto inserted = backMap.emplace(hash_part, i).second;
    assert(inserted);
    hashes.insert(hash_part);
  }

  return PathRefScanSink(std::move(hashes), std::move(backMap));
}

StorePathSet PathRefScanSink::getResultPaths() {
  /* Map the hashes found back to their store paths. */
  StorePathSet found;
  for (auto& i : getResult()) {
    auto j = backMap.find(i);
    assert(j != backMap.end());
    found.insert(j->second);
  }

  return found;
}

StorePathSet scan_for_references(Sink& to_tee, const Path& path, const StorePathSet& refs) {
  PathRefScanSink refs_sink = PathRefScanSink::fromPaths(refs);
  tee_sink_t sink{refs_sink, to_tee};

  /* Look for the hashes in the NAR dump of the path. */
  dump_path(path, sink);

  return refs_sink.getResultPaths();
}

void scan_for_references_deep(SourceAccessor& accessor, const canon_path_t& root_path,
                           const StorePathSet& refs,
                           std::function<void(FileRefScanResult)> callback) {
  // Recursive tree walker
  auto walk = [&](this auto& self, const canon_path_t& path) -> void {
    auto stat = accessor.lstat(path);

    switch (stat.type) {
      case SourceAccessor::t_regular: {
        // Create a fresh sink for each file to independently detect references.
        // RefScanSink accumulates found hashes globally - once a hash is found,
        // it remains in the result set. If we reused the same sink across files,
        // we couldn't distinguish which files contain which references, as a hash
        // found in an earlier file wouldn't be reported when found in later files.
        PathRefScanSink sink = PathRefScanSink::fromPaths(refs);

        // Scan this file by streaming its contents through the sink
        accessor.read_file(path, sink);

        // Get the references found in this file
        auto found_refs = sink.getResultPaths();

        // Report if we found anything in this file
        if (!found_refs.empty()) {
          debug("scanForReferencesDeep: found %d references in %s", found_refs.size(), path.abs());
          callback(FileRefScanResult{.filePath = path, .found_refs = std::move(found_refs)});
        }
        break;
      }

      case SourceAccessor::t_directory: {
        // Recursively scan directory contents
        auto entries = accessor.read_directory(path);
        for (const auto& [name, entryType] : entries) {
          self(path / name);
        }
        break;
      }

      case SourceAccessor::t_symlink: {
        // Create a fresh sink for the symlink target (same reason as regular files)
        PathRefScanSink sink = PathRefScanSink::fromPaths(refs);

        // Scan symlink target for references
        auto target = accessor.read_link(path);
        sink(std::string_view(target));

        // Get the references found in this symlink target
        auto found_refs = sink.getResultPaths();

        if (!found_refs.empty()) {
          debug("scanForReferencesDeep: found %d references in symlink %s", found_refs.size(),
                path.abs());
          callback(FileRefScanResult{.filePath = path, .found_refs = std::move(found_refs)});
        }
        break;
      }

      case SourceAccessor::t_char:
      case SourceAccessor::t_block:
      case SourceAccessor::t_socket:
      case SourceAccessor::t_fifo:
      case SourceAccessor::t_unknown:
      default:
        throw Error("file '%s' has an unsupported type", path.abs());
    }
  };

  // Start the recursive walk from the root
  walk(root_path);
}

std::map<canon_path_t, StorePathSet> scan_for_references_deep(SourceAccessor& accessor,
                                                        const canon_path_t& root_path,
                                                        const StorePathSet& refs) {
  std::map<canon_path_t, StorePathSet> results;

  scan_for_references_deep(accessor, root_path, refs, [&](FileRefScanResult result) {
    results[std::move(result.filePath)] = std::move(result.found_refs);
  });

  return results;
}

} // namespace nix
