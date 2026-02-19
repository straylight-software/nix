#include "nix/store/make-content-addressed.h"

#include "nix/store/references.h"

namespace nix {

std::map<StorePath, StorePath> makeContentAddressed(Store& srcStore, Store& dstStore,
                                                    const StorePathSet& storePaths) {
  StorePathSet closure;
  srcStore.computeFSClosure(storePaths, closure);

  auto paths = srcStore.topoSortPaths(closure);

  std::reverse(paths.begin(), paths.end());

  std::map<StorePath, StorePath> remappings;

  for (auto& path : paths) {
    auto pathS = srcStore.printStorePath(path);
    auto oldInfo = srcStore.queryPathInfo(path);
    std::string oldHashPart(path.hashPart());

    string_sink_t sink;
    srcStore.narFromPath(path, sink);

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
          rewrites.insert_or_assign(srcStore.printStorePath(ref),
                                    srcStore.printStorePath(replacement));
        refs.others.insert(std::move(replacement));
      }
    }

    sink.s = rewriteStrings(sink.s, rewrites);

    HashModuloSink hashModuloSink(hash_algorithm_t::SHA256, oldHashPart);
    hashModuloSink(sink.s);

    auto narModuloHash = hashModuloSink.finish().hash;

    auto info = ValidPathInfo::makeFromCA(dstStore, path.name(),
                                          FixedOutputInfo{
                                              .method = file_ingestion_method_t::NixArchive,
                                              .hash = narModuloHash,
                                              .references = std::move(refs),
                                          },
                                          Hash::dummy);

    printInfo("rewriting '%s' to '%s'", pathS, dstStore.printStorePath(info.path));

    string_sink_t sink2;
    RewritingSink rsink2(oldHashPart, std::string(info.path.hashPart()), sink2);
    rsink2(sink.s);
    rsink2.flush();

    info.narHash = hashString(hash_algorithm_t::SHA256, sink2.s);
    info.narSize = sink.s.size();

    string_source_t source(sink2.s);
    dstStore.addToStore(info, source);

    remappings.insert_or_assign(std::move(path), std::move(info.path));
  }

  return remappings;
}

StorePath makeContentAddressed(Store& srcStore, Store& dstStore, const StorePath& fromPath) {
  auto remappings = makeContentAddressed(srcStore, dstStore, StorePathSet{fromPath});
  auto i = remappings.find(fromPath);
  assert(i != remappings.end());
  return i->second;
}

} // namespace nix
