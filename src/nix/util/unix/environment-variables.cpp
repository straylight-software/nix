#include "nix/util/environment-variables.h"

#include <cstdlib>

namespace nix {

int set_env(const char* name, const char* value) {
  return ::setenv(name, value, 1);
}

std::optional<std::string> get_env_os(const std::string& key) {
  return get_env(key);
}

int set_env_os(const os_string_t& name, const os_string_t& value) {
  return set_env(name.c_str(), value.c_str());
}

} // namespace nix
