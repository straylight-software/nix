#include "nix/store/remote-fs-accessor.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <nlohmann/json.hpp>

#include "nix/util/nar-accessor.h"

namespace nix {

RemoteFSAccessor::RemoteFSAccessor(ref<Store> store, bool requireValidPath, const Path& cacheDir)
    : store(store), requireValidPath(requireValidPath), cacheDir(cacheDir) {
  if (cacheDir != "")
    createDirs(cacheDir);
}

Path RemoteFSAccessor::makeCacheFile(std::string_view hashPart, const std::string& ext) {
  assert(cacheDir != "");
  return fmt("%s/%s.%s", cacheDir, hashPart, ext);
}

ref<SourceAccessor> RemoteFSAccessor::addToCache(std::string_view hashPart, std::string&& nar) {
  if (cacheDir != "") {
    try {
      /* FIXME: do this asynchronously. */
      writeFile(makeCacheFile(hashPart, "nar"), nar);
    } catch (...) {
      ignoreExceptionExceptInterrupt();
    }
  }

  auto narAccessor = makeNarAccessor(std::move(nar));
  nars.emplace(hashPart, narAccessor);

  if (cacheDir != "") {
    try {
      nlohmann::json j = listNarDeep(*narAccessor, canon_path_t::root);
      writeFile(makeCacheFile(hashPart, "ls"), j.dump());
    } catch (...) {
      ignoreExceptionExceptInterrupt();
    }
  }

  return narAccessor;
}

std::pair<ref<SourceAccessor>, canon_path_t> RemoteFSAccessor::fetch(const canon_path_t& path) {
  auto [storePath, restPath] = store->toStorePath(store->storeDir + path.abs());
  if (requireValidPath && !store->isValidPath(storePath))
    throw InvalidPath("path '%1%' is not a valid store path", store->printStorePath(storePath));
  return {ref{accessObject(storePath)}, canon_path_t{restPath}};
}

std::shared_ptr<SourceAccessor> RemoteFSAccessor::accessObject(const StorePath& storePath) {
  auto i = nars.find(std::string(storePath.hashPart()));
  if (i != nars.end())
    return i->second;

  std::string listing;
  Path cacheFile;

  if (cacheDir != "" && nix::pathExists(cacheFile = makeCacheFile(storePath.hashPart(), "nar"))) {
    try {
      listing = nix::readFile(makeCacheFile(storePath.hashPart(), "ls"));
      auto listingJson = nlohmann::json::parse(listing);
      auto narAccessor = makeLazyNarAccessor(listingJson, seekableGetNarBytes(cacheFile));

      nars.emplace(storePath.hashPart(), narAccessor);
      return narAccessor;

    } catch (SystemError&) {
    }

    try {
      auto narAccessor = makeNarAccessor(nix::readFile(cacheFile));
      nars.emplace(storePath.hashPart(), narAccessor);
      return narAccessor;
    } catch (SystemError&) {
    }
  }

  string_sink_t sink;
  store->narFromPath(storePath, sink);
  return addToCache(storePath.hashPart(), std::move(sink.s));
}

std::optional<SourceAccessor::stat_t> RemoteFSAccessor::maybeLstat(const canon_path_t& path) {
  auto res = fetch(path);
  return res.first->maybeLstat(res.second);
}

SourceAccessor::dir_entries_t RemoteFSAccessor::readDirectory(const canon_path_t& path) {
  auto res = fetch(path);
  return res.first->readDirectory(res.second);
}

std::string RemoteFSAccessor::readFile(const canon_path_t& path) {
  auto res = fetch(path);
  return res.first->readFile(res.second);
}

std::string RemoteFSAccessor::readLink(const canon_path_t& path) {
  auto res = fetch(path);
  return res.first->readLink(res.second);
}

} // namespace nix
