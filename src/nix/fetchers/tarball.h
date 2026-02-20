#pragma once

#include <optional>

#include "nix/store/path.h"
#include "nix/util/hash.h"
#include "nix/util/ref.h"
#include "nix/util/types.h"

namespace nix {
class store_t;
struct source_accessor_t;
} // namespace nix

namespace nix::fetchers {

struct settings_t;

struct DownloadFileResult {
  store_path_t store_path;
  std::string etag;
  std::string effectiveUrl;
  std::optional<std::string> immutableUrl;
};

DownloadFileResult download_file(store_t& store, const settings_t& settings, const std::string& url,
                                 const std::string& name, const headers_t& headers = {});

struct DownloadTarballResult {
  Hash tree_hash;
  time_t last_modified;
  std::optional<std::string> immutableUrl;
  ref<source_accessor_t> accessor;
};

/**
 * Download and import a tarball into the git cache. The result is the
 * git tree hash of the root directory.
 */
ref<source_accessor_t> download_tarball(store_t& store, const settings_t& settings,
                                        const std::string& url);

} // namespace nix::fetchers
