#pragma once
///@file

#include "nix/util/types.h"

namespace nix {

enum class LogFormat {
  raw,
  rawWithLogs,
  internalJSON,
  bar,
  barWithLogs,
};

void set_log_format(const std::string& log_format_str);
void set_log_format(const LogFormat& log_format);

} // namespace nix
