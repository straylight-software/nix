#include "nix/main/common-args.h"

#include <nlohmann/json.hpp>

#include "nix/main/loggers.h"
#include "nix/main/plugin.h"
#include "nix/store/globals.h"
#include "nix/util/args/root.h"
#include "nix/util/config-global.h"
#include "nix/util/logging.h"
#include "nix/util/util.h"

namespace nix {

MixCommonArgs::MixCommonArgs(const std::string& program_name) : program_name(program_name) {
  add_flag({
      .long_name = "verbose",
      .short_name = 'v',
      .description = "Increase the logging verbosity level.",
      .category = loggingCategory,
      .handler = {[]() {
        verbosity = static_cast<verbosity_t>(std::min<std::underlying_type_t<verbosity_t>>(
            static_cast<std::underlying_type_t<verbosity_t>>(verbosity) + 1,
            static_cast<std::underlying_type_t<verbosity_t>>(lvl_vomit)));
      }},
  });

  add_flag({
      .long_name = "quiet",
      .description = "Decrease the logging verbosity level.",
      .category = loggingCategory,
      .handler = {[]() {
        verbosity = verbosity > lvl_error
                        ? static_cast<verbosity_t>(
                              static_cast<std::underlying_type_t<verbosity_t>>(verbosity) - 1)
                        : lvl_error;
      }},
  });

  add_flag({
      .long_name = "debug",
      .description = "Set the logging verbosity level to 'debug'.",
      .category = loggingCategory,
      .handler = {[]() { verbosity = lvl_debug; }},
  });

  add_flag({
      .long_name = "option",
      .description = "Set the Nix configuration setting *name* to *value* (overriding `nix.conf`).",
      .category = miscCategory,
      .labels = {"name", "value"},
      .handler = {[this](std::string name, std::string value) {
        try {
          global_config.set(name, value);
        } catch (UsageError& e) {
          if (!get_root().completions) {
            warn(e.what());
          }
        }
      }},
      .completer =
          [](add_completions_t& completions, size_t index, std::string_view prefix) {
            if (index == 0) {
              std::map<std::string, config_t::setting_info_t> settings;
              global_config.get_settings(settings);
              for (auto& s : settings) {
                if (has_prefix(s.first, prefix)) {
                  completions.add(s.first, fmt("Set the `%s` setting.", s.first));
                }
              }
            }
          },
  });

  add_flag({
      .long_name = "log-format",
      .description =
          "Set the format of log output; one of `raw`, `internal-json`, `bar` or `bar-with-logs`.",
      .category = loggingCategory,
      .labels = {"format"},
      .handler = {[](std::string format) { set_log_format(format); }},
  });

  add_flag({
      .long_name = "max-jobs",
      .short_name = 'j',
      .description = "The maximum number of parallel builds.",
      .labels = strings_t{"jobs"},
      .handler = {[=](std::string s) { settings.set("max-jobs", s); }},
  });

  std::string cat = "Options to override configuration settings";
  global_config.convert_to_args(*this, cat);

  // Backward compatibility hack: nix-env already had a --system flag.
  if (program_name == "nix-env") {
    remove_flag("system");
  }

  hide_category(cat);
}

void MixCommonArgs::initial_flags_processed() {
  init_plugins();
  plugins_inited();
}

template <typename T, typename>
void MixPrintJSON::printJSON(const T /* nlohmann::json */& json) {
  auto suspension = logger->suspend();
  if (outputPretty) {
    logger->write_to_stdout(json.dump(2));
  } else {
    logger->write_to_stdout(json.dump());
  }
}

template void MixPrintJSON::printJSON(const nlohmann::json& json);

} // namespace nix
