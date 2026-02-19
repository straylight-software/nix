#include "nix/util/current-process.h"

#include <cmath>

#include <sys/resource.h>

#include "nix/util/error.h"

namespace nix {

std::chrono::microseconds getCpuUserTime() {
  struct rusage buf;

  if (getrusage(RUSAGE_SELF, &buf) != 0) {
    throw SysError("failed to get CPU time");
  }

  std::chrono::seconds seconds(buf.ru_utime.tv_sec);
  std::chrono::microseconds microseconds(buf.ru_utime.tv_usec);

  return seconds + microseconds;
}

} // namespace nix
