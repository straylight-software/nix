#include "nix/util/users.h"

#include "nix/util/environment-variables.h"
#include "nix/util/file-system.h"
#include "nix/util/util.h"

namespace nix {

std::filesystem::path get_cache_dir() {
  auto dir = get_env("NIX_CACHE_HOME");
  if (dir) {
    return *dir;
  } else {
    auto xdg_dir = get_env("XDG_CACHE_HOME");
    if (xdg_dir) {
      return std::filesystem::path{*xdg_dir} / "nix";
    } else {
      return get_home() / ".cache" / "nix";
    }
  }
}

std::filesystem::path get_config_dir() {
  auto dir = get_env("NIX_CONFIG_HOME");
  if (dir) {
    return *dir;
  } else {
    auto xdg_dir = get_env("XDG_CONFIG_HOME");
    if (xdg_dir) {
      return std::filesystem::path{*xdg_dir} / "nix";
    } else {
      return get_home() / ".config" / "nix";
    }
  }
}

std::vector<std::filesystem::path> get_config_dirs() {
  std::filesystem::path config_home = get_config_dir();
  auto config_dirs = get_env("XDG_CONFIG_DIRS").value_or("/etc/xdg");
  auto tokens = tokenize_string<std::vector<std::string>>(config_dirs, ":");
  std::vector<std::filesystem::path> result;
  result.push_back(config_home);
  for (auto& token : tokens) {
    result.push_back(std::filesystem::path{token} / "nix");
  }
  return result;
}

std::filesystem::path get_data_dir() {
  auto dir = get_env("NIX_DATA_HOME");
  if (dir) {
    return *dir;
  } else {
    auto xdg_dir = get_env("XDG_DATA_HOME");
    if (xdg_dir) {
      return std::filesystem::path{*xdg_dir} / "nix";
    } else {
      return get_home() / ".local" / "share" / "nix";
    }
  }
}

std::filesystem::path get_state_dir() {
  auto dir = get_env("NIX_STATE_HOME");
  if (dir) {
    return *dir;
  } else {
    auto xdg_dir = get_env("XDG_STATE_HOME");
    if (xdg_dir) {
      return std::filesystem::path{*xdg_dir} / "nix";
    } else {
      return get_home() / ".local" / "state" / "nix";
    }
  }
}

std::filesystem::path create_nix_state_dir() {
  std::filesystem::path dir = get_state_dir();
  create_dirs(dir);
  return dir;
}

std::string expand_tilde(std::string_view path) {
  // TODO: expand ~user ?
  auto tilde = path.substr(0, 2);
  if (tilde == "~/" || tilde == "~") {
    return get_home().string() + std::string(path.substr(1));
  } else {
    return std::string(path);
}
}

} // namespace nix
