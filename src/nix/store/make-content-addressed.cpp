#include "nix/store/make-content-addressed.h"

#include "nix/store/references.h"

namespace nix {

std::map<StorePath, StorePath> make_content_addressed(Store& src_store, Store& dst_store,
                                                    const StorePathSet& store_paths) {
  StorePathSet closure;
  src_store.computeFSClosure(store_paths, closure);

  auto paths = src_store.topoSortPaths(closure);

  std::reverse(paths.begin(), paths.end());

  std::map<StorePath, StorePath> remappings;

  for (auto& path : paths) {
    auto path_s = src_store.printStorePath(path);
    auto oldInfo = src_store.queryPathInfo(path);
    std::string oldHashPart(path.hash_part());

    string_sink_t sink;
    src_store.nar_from_path(path, sink);

    string_map_t rewrites;

    StoreReferences refs;
    for (auto& ref : oldInfo->references) {
      if (ref == path)
        refs.self = true;
      else {
        auto i = remappings.find(ref);
        auto replacement = i != remappings.end() ? i->second : ref;
        // FIXME: warn about unremapped paths?
        if (replacement != ref)
          rewrites.insert_or_assign(src_store.printStorePath(ref),
                                    src_store.printStorePath(replacement));
        refs.others.insert(std::move(replacement));
      }
    }

    sink.str() = rewrite_strings(sink.str(), rewrites);

    HashModuloSink hashModuloSink(hash_algorithm_t::SHA256, oldHashPart);
    hashModuloSink(sink.str());

    auto narModuloHash = hashModuloSink.finish().hash;

    auto info = ValidPathInfo::makeFromCA(dst_store, path.name(),
                                          FixedOutputInfo{
                                              .method = file_ingestion_method_t::nix_archive,
                                              .hash = narModuloHash,
                                              .references = std::move(refs),
                                          },
                                          Hash::dummy);

    printInfo("rewriting '%s' to '%s'", path_s, dst_store.printStorePath(info.path));

    string_sink_t sink2;
    RewritingSink rsink2(oldHashPart, std::string(info.path.hash_part()), sink2);
    rsink2(sink.str());
    rsink2.flush();

    info.nar_hash = hash_string(hash_algorithm_t::SHA256, sink2.str());
    info.nar_size = sink.str().size();

    string_source_t source(sink2.str());
    dst_store.add_to_store(info, source);

    remappings.insert_or_assign(std::move(path), std::move(info.path));
  }

  return remappings;
}

StorePath make_content_addressed(Store& src_store, Store& dst_store, const StorePath& from_path) {
  auto remappings = make_content_addressed(src_store, dst_store, StorePathSet{from_path});
  auto i = remappings.find(from_path);
  assert(i != remappings.end());
  return i->second;
}

} // namespace nix
