#pragma once
///@file

#include "nix/store/store-api.h"
#include "nix/util/ref.h"
#include "nix/util/source-accessor.h"

namespace nix {

class RemoteFSAccessor : public source_accessor_t {
  ref<store_t> store;

  std::map<std::string, ref<source_accessor_t>> nars;

  bool require_valid_path;

  Path cache_dir;

  std::pair<ref<source_accessor_t>, canon_path_t> fetch(const canon_path_t& path);

  friend struct binary_cache_store;

  Path makeCacheFile(std::string_view hash_part, const std::string& ext);

  ref<source_accessor_t> addToCache(std::string_view hash_part, std::string&& nar);

public:
  /**
   * @return nullptr if the store does not contain any object at that path.
   */
  std::shared_ptr<source_accessor_t> accessObject(const store_path_t& path);

  RemoteFSAccessor(ref<store_t> store, bool require_valid_path = true,
                   const /* FIXME: use std::optional */ Path& cache_dir = "");

  std::optional<stat_t> maybe_lstat(const canon_path_t& path) override;

  dir_entries_t read_directory(const canon_path_t& path) override;

  std::string read_file(const canon_path_t& path) override;

  std::string read_link(const canon_path_t& path) override;
};

} // namespace nix
