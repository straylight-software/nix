#include "nix/main/loggers.h"

#include "nix/main/progress-bar.h"
#include "nix/util/environment-variables.h"

namespace nix {

LogFormat default_log_format = LogFormat::raw;

LogFormat parse_log_format(const std::string& log_format_str) {
  if (log_format_str == "raw" || get_env("NIX_GET_COMPLETIONS"))
    return LogFormat::raw;
  else if (log_format_str == "raw-with-logs")
    return LogFormat::rawWithLogs;
  else if (log_format_str == "internal-json")
    return LogFormat::internalJSON;
  else if (log_format_str == "bar")
    return LogFormat::bar;
  else if (log_format_str == "bar-with-logs")
    return LogFormat::barWithLogs;
  throw Error("option 'log-format' has an invalid value '%s'", log_format_str);
}

std::unique_ptr<logger_t> make_default_logger() {
  switch (default_log_format) {
    case LogFormat::raw:
      return make_simple_logger(false);
    case LogFormat::rawWithLogs:
      return make_simple_logger(true);
    case LogFormat::internalJSON:
      return make_json_logger(get_standard_error());
    case LogFormat::bar:
      return make_progress_bar();
    case LogFormat::barWithLogs: {
      auto logger = make_progress_bar();
      logger->set_print_build_logs(true);
      return logger;
    }
    default:
      unreachable();
  }
}

void set_log_format(const std::string& log_format_str) {
  set_log_format(parse_log_format(log_format_str));
}

void set_log_format(const LogFormat& log_format) {
  default_log_format = log_format;
  logger = make_default_logger();
}

} // namespace nix
