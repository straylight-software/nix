#pragma once
///@file

#include "nix/util/args.h"
#include "nix/util/repair-flag.h"

namespace nix {

// static constexpr auto commonArgsCategory = "Miscellaneous common options";
static constexpr auto loggingCategory = "Logging-related options";
static constexpr auto miscCategory = "Miscellaneous global options";

class MixCommonArgs : public virtual args_t {
  void initial_flags_processed() override;

public:
  std::string program_name;
  MixCommonArgs(const std::string& program_name);

protected:
  virtual void plugins_inited() {}
};

struct MixDryRun : virtual args_t {
  bool dry_run = false;

  MixDryRun() {
    add_flag({
        .long_name = "dry-run",
        .description = "Show what this command would do without doing it.",
        .handler = {&dry_run, true},
    });
  }
};

/**
 * commands_t that can print JSON according to the
 * `--pretty`/`--no-pretty` flag.
 *
 * This is distinct from MixJSON, because for some commands,
 * JSON outputs is not optional.
 */
struct MixPrintJSON : virtual args_t {
  bool outputPretty = isatty(STDOUT_FILENO);

  MixPrintJSON() {
    add_flag({
        .long_name = "pretty",
        .description =
            R"(
                    Print multi-line, indented JSON output for readability.

                    Default: indent if output is to a terminal.

                    This option is only effective when `--json` is also specified.
                )",
        .handler = {&outputPretty, true},
    });
    add_flag({
        .long_name = "no-pretty",
        .description =
            R"(
                    Print compact JSON output on a single line, even when the output is a terminal.
                    Some commands may print multiple JSON objects on separate lines.

                    See `--pretty`.
                )",
        .handler = {&outputPretty, false},
    });
  };

  /**
   * Print an `nlohmann::json` to stdout
   *
   * - respecting `--pretty` / `--no-pretty`.
   * - suspending the progress bar
   *
   * This is a template to avoid accidental coercions from `string` to `json` in the caller,
   * to avoid mistakenly passing an already serialized JSON to this function.
   *
   * It is not recommended to print a JSON string - see the JSON guidelines
   * about extensibility, https://nix.dev/manual/nix/development/development/json-guideline.html -
   * but you _can_ print a sole JSON string by explicitly coercing it to
   * `nlohmann::json` first.
   */
  template <typename T, typename = std::enable_if_t<std::is_same_v<T, nlohmann::json>>>
  void printJSON(const T& json);
};

/** Optional JSON support via `--json` flag */
struct MixJSON : virtual args_t, virtual MixPrintJSON {
  bool json = false;

  MixJSON() {
    add_flag({
        .long_name = "json",
        .description =
            "Produce output in JSON format, suitable for consumption by another program.",
        .handler = {&json, true},
    });
  }
};

struct MixRepair : virtual args_t {
  RepairFlag repair = NoRepair;

  MixRepair() {
    add_flag({
        .long_name = "repair",
        .description = "During evaluation, rewrite missing or corrupted files in the Nix store. "
                       "During building, rebuild missing or corrupted store paths.",
        .category = miscCategory,
        .handler = {&repair, Repair},
    });
  }
};

} // namespace nix
