#include "nix/store/remote-fs-accessor.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <nlohmann/json.hpp>

#include "nix/util/nar-accessor.h"

namespace nix {

RemoteFSAccessor::RemoteFSAccessor(ref<Store> store, bool require_valid_path, const Path& cache_dir)
    : store(store), require_valid_path(require_valid_path), cache_dir(cache_dir) {
  if (cache_dir != "")
    create_dirs(cache_dir);
}

Path RemoteFSAccessor::makeCacheFile(std::string_view hash_part, const std::string& ext) {
  assert(cache_dir != "");
  return fmt("%s/%s.%s", cache_dir, hash_part, ext);
}

ref<SourceAccessor> RemoteFSAccessor::addToCache(std::string_view hash_part, std::string&& nar) {
  if (cache_dir != "") {
    try {
      /* FIXME: do this asynchronously. */
      write_file(makeCacheFile(hash_part, "nar"), nar);
    } catch (...) {
      ignore_exception_except_interrupt();
    }
  }

  auto narAccessor = make_nar_accessor(std::move(nar));
  nars.emplace(hash_part, narAccessor);

  if (cache_dir != "") {
    try {
      nlohmann::json j = list_nar_deep(*narAccessor, canon_path_t::root);
      write_file(makeCacheFile(hash_part, "ls"), j.dump());
    } catch (...) {
      ignore_exception_except_interrupt();
    }
  }

  return narAccessor;
}

std::pair<ref<SourceAccessor>, canon_path_t> RemoteFSAccessor::fetch(const canon_path_t& path) {
  auto [store_path, restPath] = store->toStorePath(store->store_dir + path.abs());
  if (require_valid_path && !store->isValidPath(store_path))
    throw InvalidPath("path '%1%' is not a valid store path", store->printStorePath(store_path));
  return {ref{accessObject(store_path)}, canon_path_t{restPath}};
}

std::shared_ptr<SourceAccessor> RemoteFSAccessor::accessObject(const StorePath& store_path) {
  auto i = nars.find(std::string(store_path.hash_part()));
  if (i != nars.end())
    return i->second;

  std::string listing;
  Path cacheFile;

  if (cache_dir != "" && nix::path_exists(cacheFile = makeCacheFile(store_path.hash_part(), "nar"))) {
    try {
      listing = nix::read_file(makeCacheFile(store_path.hash_part(), "ls"));
      auto listingJson = nlohmann::json::parse(listing);
      auto narAccessor = make_lazy_nar_accessor(listingJson, seekable_get_nar_bytes(cacheFile));

      nars.emplace(store_path.hash_part(), narAccessor);
      return narAccessor;

    } catch (SystemError&) {
    }

    try {
      auto narAccessor = make_nar_accessor(nix::read_file(cacheFile));
      nars.emplace(store_path.hash_part(), narAccessor);
      return narAccessor;
    } catch (SystemError&) {
    }
  }

  string_sink_t sink;
  store->nar_from_path(store_path, sink);
  return addToCache(store_path.hash_part(), std::move(sink.str()));
}

std::optional<SourceAccessor::stat_t> RemoteFSAccessor::maybe_lstat(const canon_path_t& path) {
  auto res = fetch(path);
  return res.first->maybe_lstat(res.second);
}

SourceAccessor::dir_entries_t RemoteFSAccessor::read_directory(const canon_path_t& path) {
  auto res = fetch(path);
  return res.first->read_directory(res.second);
}

std::string RemoteFSAccessor::read_file(const canon_path_t& path) {
  auto res = fetch(path);
  return res.first->read_file(res.second);
}

std::string RemoteFSAccessor::read_link(const canon_path_t& path) {
  auto res = fetch(path);
  return res.first->read_link(res.second);
}

} // namespace nix
