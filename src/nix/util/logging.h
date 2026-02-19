#pragma once
///@file

#include <filesystem>

#include <nlohmann/json_fwd.hpp>

#include "nix/util/configuration.h"
#include "nix/util/error.h"
#include "nix/util/file-descriptor.h"
#include "nix/util/finally.h"

namespace nix {

typedef enum {
  act_unknown = 0,
  act_copy_path = 100,
  act_file_transfer = 101,
  act_realise = 102,
  act_copy_paths = 103,
  act_builds = 104,
  act_build = 105,
  act_optimise_store = 106,
  act_verify_paths = 107,
  act_substitute = 108,
  act_query_path_info = 109,
  act_post_build_hook = 110,
  act_build_waiting = 111,
  act_fetch_tree = 112,
} activity_type_t;

typedef enum {
  res_file_linked = 100,
  res_build_log_line = 101,
  res_untrusted_path = 102,
  res_corrupted_path = 103,
  res_set_phase = 104,
  res_progress = 105,
  res_set_expected = 106,
  res_post_build_log_line = 107,
  res_fetch_status = 108,
  res_hash_mismatch = 109,
  res_build_result = 110,
} result_type_t;

typedef uint64_t activity_id_t;

struct logger_settings_t : config_t {
  setting_t<bool> show_trace{this, false, "show-trace",
                          R"(
          Whether Nix should print out a stack trace in case of Nix
          expression evaluation errors.
        )"};

  setting_t<std::optional<std::filesystem::path>> json_log_path{this,
                                                            {},
                                                            "json-log-path",
                                                            R"(
          A file or Unix domain socket to which JSON records of Nix's log output are
          written, in the same format as `--log-format internal-json`
          (without the `@nix ` prefixes on each line).
          Concurrent writes to the same file by multiple Nix processes are not supported and
          may result in interleaved or corrupted log records.
        )"};
};

extern logger_settings_t logger_settings;

class logger_t {
  friend struct activity_t;

public:
  struct field_t {
    // FIXME: use std::variant.
    enum { t_int = 0, t_string = 1 } type;

    uint64_t i = 0;
    std::string s;

    field_t(const std::string& s) : type(t_string), s(s) {}

    field_t(const char* s) : type(t_string), s(s) {}

    field_t(const uint64_t& i) : type(t_int), i(i) {}
  };

  typedef std::vector<field_t> fields_t;

  virtual ~logger_t() {}

  virtual void stop() {};

  /**
   * Guard object to resume the logger when done.
   */
  struct suspension_t {
    finally_t<std::function<void()>> _finalize;
  };

  suspension_t suspend();

  std::optional<suspension_t> suspend_if(bool cond);

  virtual void pause() {};
  virtual void resume() {};

  // Whether the logger prints the whole build log
  virtual bool is_verbose() { return false; }

  virtual void log(verbosity_t lvl, std::string_view s) = 0;

  void log(std::string_view s) { log(lvl_info, s); }

  virtual void log_ei(const error_info_t& ei) = 0;

  void log_ei(verbosity_t lvl, error_info_t ei) {
    ei.level = lvl;
    log_ei(ei);
  }

  virtual void warn(const std::string& msg);

  virtual void start_activity(activity_id_t act, verbosity_t lvl, activity_type_t type, const std::string& s,
                             const fields_t& fields, activity_id_t parent) {};

  virtual void stop_activity(activity_id_t act) {};

  virtual void result(activity_id_t act, result_type_t type, const fields_t& fields) {};

  virtual void result(activity_id_t act, result_type_t type, const nlohmann::json& json) {};

  virtual void write_to_stdout(std::string_view s);

  template <typename... Args>
  inline void cout(const Args&... args) {
    write_to_stdout(fmt(args...));
  }

  virtual std::optional<char> ask(std::string_view s) { return {}; }

  virtual void set_print_build_logs(bool print_build_logs) {}
};

/**
 * A variadic template that does nothing.
 *
 * Useful to call a function with each argument in a parameter pack.
 */
struct nop {
  template <typename... T>
  nop(T...) {}
};

activity_id_t get_cur_activity();
void set_cur_activity(const activity_id_t activity_id);

struct activity_t {
  logger_t& logger;

  const activity_id_t id;

  activity_t(logger_t& logger, verbosity_t lvl, activity_type_t type, const std::string& s = "",
           const logger_t::fields_t& fields = {}, activity_id_t parent = get_cur_activity());

  activity_t(logger_t& logger, activity_type_t type, const logger_t::fields_t& fields = {},
           activity_id_t parent = get_cur_activity())
      : activity_t(logger, lvl_error, type, "", fields, parent) {};

  activity_t(const activity_t& act) = delete;

  ~activity_t();

  void progress(uint64_t done = 0, uint64_t expected = 0, uint64_t running = 0,
                uint64_t failed = 0) const {
    result(res_progress, done, expected, running, failed);
  }

  void set_expected(activity_type_t type2, uint64_t expected) const {
    result(res_set_expected, type2, expected);
  }

  void result(result_type_t type, const nlohmann::json& json) const { logger.result(id, type, json); }

  template <typename... Args>
  void result(result_type_t type, const Args&... args) const {
    logger_t::fields_t fields;
    nop{(fields.emplace_back(logger_t::field_t(args)), 1)...};
    result(type, fields);
  }

  void result(result_type_t type, const logger_t::fields_t& fields) const {
    logger.result(id, type, fields);
  }

  friend class logger_t;
};

struct push_activity_t {
  const activity_id_t prev_act;

  push_activity_t(activity_id_t act) : prev_act(get_cur_activity()) { set_cur_activity(act); }

  ~push_activity_t() { set_cur_activity(prev_act); }
};

extern std::unique_ptr<logger_t> logger;

std::unique_ptr<logger_t> make_simple_logger(bool print_build_logs = true);

/**
 * Create a logger that sends log messages to `main_logger` and the
 * list of loggers in `extra_loggers`. Only `main_logger` is used for
 * writing to stdout and getting user input.
 */
std::unique_ptr<logger_t> make_tee_logger(std::unique_ptr<logger_t> main_logger,
                                      std::vector<std::unique_ptr<logger_t>>&& extra_loggers);

std::unique_ptr<logger_t> make_json_logger(descriptor_t fd, bool include_nix_prefix = true);

std::unique_ptr<logger_t> make_json_logger(const std::filesystem::path& path,
                                       bool include_nix_prefix = true);

void apply_json_logger();

/**
 * @param source A noun phrase describing the source of the message, e.g. "the builder".
 */
std::optional<nlohmann::json> parse_json_message(const std::string& msg, std::string_view source);

/**
 * @param source A noun phrase describing the source of the message, e.g. "the builder".
 */
bool handle_json_log_message(nlohmann::json& json, const activity_t& act,
                          std::map<activity_id_t, activity_t>& activities, std::string_view source,
                          bool trusted);

/**
 * @param source A noun phrase describing the source of the message, e.g. "the builder".
 */
bool handle_json_log_message(const std::string& msg, const activity_t& act,
                          std::map<activity_id_t, activity_t>& activities, std::string_view source,
                          bool trusted);

/**
 * suppress msgs > this
 */
extern verbosity_t verbosity;

/**
 * Print a message with the standard error_info_t format.
 * In general, use these 'log' macros for reporting problems that may require user
 * intervention or that need more explanation.  use the 'print' macros for more
 * lightweight status messages.
 */
#define logErrorInfo(level, errorInfo...)                                                          \
  do {                                                                                             \
    if ((level) <= nix::verbosity) {                                                               \
      logger->log_ei((level), errorInfo);                                                           \
    }                                                                                              \
  } while (0)

#define logError(errorInfo...) logErrorInfo(lvl_error, errorInfo)
#define logWarning(errorInfo...) logErrorInfo(lvl_warn, errorInfo)

/**
 * Print a string message if the current log level is at least the specified
 * level. Note that this has to be implemented as a macro to ensure that the
 * arguments are evaluated lazily.
 */
#define printMsgUsing(loggerParam, level, args...)                                                 \
  do {                                                                                             \
    auto __lvl = level;                                                                            \
    if (__lvl <= nix::verbosity) {                                                                 \
      loggerParam->log(__lvl, fmt(args));                                                          \
    }                                                                                              \
  } while (0)
#define printMsg(level, args...) printMsgUsing(logger, level, args)

#define printError(args...) printMsg(lvl_error, args)
#define notice(args...) printMsg(lvl_notice, args)
#define printInfo(args...) printMsg(lvl_info, args)
#define printTalkative(args...) printMsg(lvl_talkative, args)
#define debug(args...) printMsg(lvl_debug, args)
#define vomit(args...) printMsg(lvl_vomit, args)

/**
 * if verbosity >= lvl_warn, print a message with a yellow 'warning:' prefix.
 */
template <typename... Args>
inline void warn(const std::string& fs, const Args&... args) {
  boost::format f(fs);
  format_helper(f, args...);
  logger->warn(f.str());
}

#define warnOnce(have_warned, args...)                                                              \
  if (!have_warned) {                                                                               \
    have_warned = true;                                                                             \
    warn(args);                                                                                    \
  }

void write_to_stderr(std::string_view s);

} // namespace nix
