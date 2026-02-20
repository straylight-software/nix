#include "nix/util/environment-variables.h"

#include "nix/util/util.h"

extern char** environ __attribute__((weak));

namespace nix {

std::optional<std::string> get_env(const std::string& key) {
  char* value = getenv(key.c_str());
  if (!value) {
    return {};
  }
  return std::string(value);
}

std::optional<std::string> get_env_non_empty(const std::string& key) {
  auto value = get_env(key);
  if (value == "") {
    return {};
  }
  return value;
}

string_map_t get_env() {
  string_map_t env;
  for (size_t i = 0; environ[i]; ++i) {
    auto s = environ[i];
    auto eq = strchr(s, '=');
    if (!eq) {
      // invalid env, just keep going
      continue;
    }
    env.emplace(std::string(s, eq), std::string(eq + 1));
  }
  return env;
}

void clear_env() {
  for (auto& name : get_env()) {
    unsetenv(name.first.c_str());
  }
}

void replace_env(const string_map_t& new_env) {
  clear_env();
  for (auto& new_env_var : new_env) {
    set_env(new_env_var.first.c_str(), new_env_var.second.c_str());
  }
}

} // namespace nix
