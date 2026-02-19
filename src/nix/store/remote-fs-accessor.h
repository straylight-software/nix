#pragma once
///@file

#include "nix/store/store-api.h"
#include "nix/util/ref.h"
#include "nix/util/source-accessor.h"

namespace nix {

class RemoteFSAccessor : public SourceAccessor {
  ref<Store> store;

  std::map<std::string, ref<SourceAccessor>> nars;

  bool require_valid_path;

  Path cache_dir;

  std::pair<ref<SourceAccessor>, canon_path_t> fetch(const canon_path_t& path);

  friend struct binary_cache_store;

  Path makeCacheFile(std::string_view hash_part, const std::string& ext);

  ref<SourceAccessor> addToCache(std::string_view hash_part, std::string&& nar);

public:
  /**
   * @return nullptr if the store does not contain any object at that path.
   */
  std::shared_ptr<SourceAccessor> accessObject(const StorePath& path);

  RemoteFSAccessor(ref<Store> store, bool require_valid_path = true,
                   const /* FIXME: use std::optional */ Path& cache_dir = "");

  std::optional<stat_t> maybe_lstat(const canon_path_t& path) override;

  dir_entries_t read_directory(const canon_path_t& path) override;

  std::string read_file(const canon_path_t& path) override;

  std::string read_link(const canon_path_t& path) override;
};

} // namespace nix
