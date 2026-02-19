#pragma once

#include "source-accessor.h"

namespace nix {

struct mounted_source_accessor_t : SourceAccessor {
  virtual void mount(canon_path_t mountPoint, ref<SourceAccessor> accessor) = 0;

  /**
   * Return the accessor mounted on `mountPoint`, or `nullptr` if
   * there is no such mount point.
   */
  virtual std::shared_ptr<SourceAccessor> getMount(canon_path_t mountPoint) = 0;
};

ref<mounted_source_accessor_t>
makeMountedSourceAccessor(std::map<canon_path_t, ref<SourceAccessor>> mounts);

} // namespace nix
