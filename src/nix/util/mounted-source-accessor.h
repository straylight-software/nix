#pragma once

#include "source-accessor.h"

namespace nix {

struct mounted_source_accessor_t : SourceAccessor {
  virtual void mount(canon_path_t mount_point, ref<SourceAccessor> accessor) = 0;

  /**
   * Return the accessor mounted on `mount_point`, or `nullptr` if
   * there is no such mount point.
   */
  virtual std::shared_ptr<SourceAccessor> get_mount(canon_path_t mount_point) = 0;
};

ref<mounted_source_accessor_t>
make_mounted_source_accessor(std::map<canon_path_t, ref<SourceAccessor>> mounts);

} // namespace nix
