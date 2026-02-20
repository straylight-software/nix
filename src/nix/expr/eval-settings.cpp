#include "nix/expr/eval-settings.h"

#include "nix/expr/eval.h"
#include "nix/store/globals.h"
#include "nix/store/profiles.h"
#include "nix/util/users.h"

namespace nix {

/* Very hacky way to parse $NIX_PATH, which is colon-separated, but
   can contain URLs (e.g. "nixpkgs=https://bla...:foo=https://"). */
strings_t eval_settings_t::parseNixPath(const std::string& s) {
  strings_t res;

  auto p = s.begin();

  while (p != s.end()) {
    auto start = p;
    auto start2 = p;

    while (p != s.end() && *p != ':') {
      if (*p == '=')
        start2 = p + 1;
      ++p;
    }

    if (p == s.end()) {
      if (p != start)
        res.push_back(std::string(start, p));
      break;
    }

    if (*p == ':') {
      auto prefix = std::string(start2, s.end());
      if (eval_settings_t::isPseudoUrl(prefix) || has_prefix(prefix, "flake:")) {
        ++p;
        while (p != s.end() && *p != ':')
          ++p;
      }
      res.push_back(std::string(start, p));
      if (p == s.end())
        break;
    }

    ++p;
  }

  return res;
}

eval_settings_t::eval_settings_t(bool& readOnlyMode, eval_settings_t::LookupPathHooks lookupPathHooks)
    : readOnlyMode{readOnlyMode}, lookupPathHooks{lookupPathHooks} {
  auto var = get_env("NIX_ABORT_ON_WARN");
  if (var && (var == "1" || var == "yes" || var == "true"))
    builtinsAbortOnWarn = true;
}

strings_t eval_settings_t::getDefaultNixPath() {
  strings_t res;
  auto add = [&](const std::filesystem::path& p, const std::string& s = std::string()) {
    if (std::filesystem::exists(p)) {
      if (s.empty()) {
        res.push_back(p.string());
      } else {
        res.push_back(s + "=" + p.string());
      }
    }
  };

  add(std::filesystem::path{get_nix_def_expr()} / "channels");
  add(root_channels_dir() / "nixpkgs", "nixpkgs");
  add(root_channels_dir());

  return res;
}

bool eval_settings_t::isPseudoUrl(std::string_view s) {
  if (s.compare(0, 8, "channel:") == 0)
    return true;
  size_t pos = s.find("://");
  if (pos == std::string::npos)
    return false;
  std::string scheme(s, 0, pos);
  return scheme == "http" || scheme == "https" || scheme == "file" || scheme == "channel" ||
         scheme == "git" || scheme == "s3" || scheme == "ssh";
}

std::string eval_settings_t::resolvePseudoUrl(std::string_view url) {
  if (has_prefix(url, "channel:")) {
    auto realUrl = "https://channels.nixos.org/" + std::string(url.substr(8)) + "/nixexprs.tar.xz";
    static bool have_warned = false;
    warnOnce(have_warned,
             "Channels are deprecated in favor of flakes in Determinate Nix. "
             "Instead of '%s', use '%s'. "
             "See https://zero-to-nix.com for a guide to Nix flakes. "
             "For details and to offer feedback on the deprecation process, see: "
             "https://github.com/DeterminateSystems/nix-src/issues/34.",
             url, realUrl);
    return realUrl;
  } else
    return std::string(url);
}

const std::string& eval_settings_t::getCurrentSystem() const {
  const auto& evalSystem = currentSystem.get();
  return evalSystem != "" ? evalSystem : settings.thisSystem.get();
}

std::filesystem::path get_nix_def_expr() {
  return settings.useXDGBaseDirectories ? get_state_dir() / "defexpr" : get_home() / ".nix-defexpr";
}

} // namespace nix
