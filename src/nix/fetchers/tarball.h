#pragma once

#include <optional>

#include "nix/store/path.h"
#include "nix/util/hash.h"
#include "nix/util/ref.h"
#include "nix/util/types.h"

namespace nix {
class Store;
struct SourceAccessor;
} // namespace nix

namespace nix::fetchers {

struct settings_t;

struct DownloadFileResult {
  StorePath store_path;
  std::string etag;
  std::string effectiveUrl;
  std::optional<std::string> immutableUrl;
};

DownloadFileResult download_file(Store& store, const settings_t& settings, const std::string& url,
                                const std::string& name, const headers_t& headers = {});

struct DownloadTarballResult {
  Hash tree_hash;
  time_t last_modified;
  std::optional<std::string> immutableUrl;
  ref<SourceAccessor> accessor;
};

/**
 * Download and import a tarball into the git cache. The result is the
 * git tree hash of the root directory.
 */
ref<SourceAccessor> download_tarball(Store& store, const settings_t& settings, const std::string& url);

} // namespace nix::fetchers
