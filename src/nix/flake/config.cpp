#include <cctype>
#include <filesystem>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <assert.h>
#include <stdint.h>

#include <nlohmann/json.hpp>
#include <nlohmann/json_fwd.hpp>

#include "nix/flake/flake.h"
#include "nix/flake/settings.h"
#include "nix/util/ansicolor.h"
#include "nix/util/config-global.h"
#include "nix/util/configuration.h"
#include "nix/util/file-system.h"
#include "nix/util/fmt.h"
#include "nix/util/logging.h"
#include "nix/util/strings.h"
#include "nix/util/types.h"
#include "nix/util/users.h"
#include "nix/util/util.h"

namespace nix::flake {

// setting name -> setting value -> allow or ignore.
typedef std::map<std::string, std::map<std::string, bool>> TrustedList;

std::filesystem::path trusted_list_path() {
  return get_data_dir() / "trusted-settings.json";
}

static TrustedList read_trusted_list() {
  auto path = trusted_list_path();
  if (!path_exists(path))
    return {};
  auto json = nlohmann::json::parse(read_file(path));
  return json;
}

static void write_trusted_list(const TrustedList& trusted_list) {
  auto path = trusted_list_path();
  create_dirs(path.parent_path());
  write_file(path, nlohmann::json(trusted_list).dump());
}

void ConfigFile::apply(const settings_t& flake_settings) {
  string_set_t whitelist{"bash-prompt",    "bash-prompt-prefix",       "bash-prompt-suffix",
                         "flake-registry", "commit-lock-file-summary", "commit-lockfile-summary"};

  for (auto& [name, value] : settings) {
    auto base_name = has_prefix(name, "extra-") ? std::string(name, 6) : name;

    // FIXME: Move into libutil/config.cc.
    std::string valueS;
    if (auto* s = std::get_if<std::string>(&value))
      valueS = *s;
    else if (auto* n = std::get_if<int64_t>(&value))
      valueS = fmt("%d", *n);
    else if (auto* b = std::get_if<explicit_t<bool>>(&value))
      valueS = b->t ? "true" : "false";
    else if (auto ss = std::get_if<std::vector<std::string>>(&value))
      valueS = drop_empty_init_then_concat_strings_sep(" ", *ss); // FIXME: evil
    else
      assert(false);

    if (!whitelist.count(base_name) && !flake_settings.acceptFlakeConfig) {
      bool trusted = false;
      auto trusted_list = read_trusted_list();
      auto tlname = get(trusted_list, name);
      if (auto saved = tlname ? get(*tlname, valueS) : nullptr) {
        trusted = *saved;
        printInfo(
            "Using saved setting for '%s = %s' from ~/.local/share/nix/trusted-settings.json.",
            name, valueS);
      } else {
        // FIXME: filter ANSI escapes, newlines, \r, etc.
        if (std::tolower(
                logger
                    ->ask(fmt(
                        "do you want to allow configuration setting '%s' to be set to '" ANSI_RED
                        "%s" ANSI_NORMAL "' (y/N)?",
                        name, valueS))
                    .value_or('n')) == 'y') {
          trusted = true;
        }
        if (std::tolower(logger
                             ->ask(fmt("do you want to permanently mark this value as %s (y/N)?",
                                       trusted ? "trusted" : "untrusted"))
                             .value_or('n')) == 'y') {
          trusted_list[name][valueS] = trusted;
          write_trusted_list(trusted_list);
        }
      }
      if (!trusted) {
        warn("ignoring untrusted flake configuration setting '%s'.\nPass '%s' to trust it", name,
             "--accept-flake-config");
        continue;
      }
    }

    global_config.set(name, valueS);
  }
}

} // namespace nix::flake
