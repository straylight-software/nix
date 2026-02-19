#pragma once
///@file

#include <optional>
#include <string_view>

#include "nix/util/util.h"

namespace nix {

/**
 * If `separator` is found, we return the portion of the string before the
 * separator, and modify the string argument to contain only the part after the
 * separator. Otherwise, we return `std::nullopt`, and we leave the argument
 * string alone.
 */
static inline std::optional<std::string_view> split_prefix_to(std::string_view& string,
                                                            char separator) {
  auto sep_instance = string.find(separator);

  if (sep_instance != std::string_view::npos) {
    auto prefix = string.substr(0, sep_instance);
    string.remove_prefix(sep_instance + 1);
    return prefix;
  }

  return std::nullopt;
}

static inline bool split_prefix(std::string_view& string, std::string_view prefix) {
  bool res = has_prefix(string, prefix);
  if (res)
    string.remove_prefix(prefix.length());
  return res;
}

} // namespace nix
