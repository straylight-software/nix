#pragma once
///@file

#include "nix/store/nar-info.h"
#include "nix/store/realisation.h"
#include "nix/util/ref.h"

namespace nix {

class NarInfoDiskCache {
public:
  typedef enum { oValid, oInvalid, oUnknown } Outcome;

  virtual ~NarInfoDiskCache() {}

  virtual int createCache(const std::string& uri, const Path& store_dir, bool want_mass_query,
                          int priority) = 0;

  struct CacheInfo {
    int id;
    bool want_mass_query;
    int priority;
  };

  virtual std::optional<CacheInfo> upToDateCacheExists(const std::string& uri) = 0;

  virtual std::pair<Outcome, std::shared_ptr<nar_info_t>>
  lookupNarInfo(const std::string& uri, const std::string& hash_part) = 0;

  virtual void upsertNarInfo(const std::string& uri, const std::string& hash_part,
                             std::shared_ptr<const valid_path_info_t> info) = 0;

  virtual void upsertRealisation(const std::string& uri, const realisation_t& realisation) = 0;
  virtual void upsertAbsentRealisation(const std::string& uri, const DrvOutput& id) = 0;
  virtual std::pair<Outcome, std::shared_ptr<realisation_t>>
  lookupRealisation(const std::string& uri, const DrvOutput& id) = 0;
};

/**
 * Return a singleton cache object that can be used concurrently by
 * multiple threads.
 */
ref<NarInfoDiskCache> get_nar_info_disk_cache();

ref<NarInfoDiskCache> get_test_nar_info_disk_cache(Path db_path);

} // namespace nix
