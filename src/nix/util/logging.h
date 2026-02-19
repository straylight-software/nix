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
  actUnknown = 0,
  actCopyPath = 100,
  actFileTransfer = 101,
  actRealise = 102,
  actCopyPaths = 103,
  actBuilds = 104,
  actBuild = 105,
  actOptimiseStore = 106,
  actVerifyPaths = 107,
  actSubstitute = 108,
  actQueryPathInfo = 109,
  actPostBuildHook = 110,
  actBuildWaiting = 111,
  actFetchTree = 112,
} activity_type_t;

typedef enum {
  resFileLinked = 100,
  resBuildLogLine = 101,
  resUntrustedPath = 102,
  resCorruptedPath = 103,
  resSetPhase = 104,
  resProgress = 105,
  resSetExpected = 106,
  resPostBuildLogLine = 107,
  resFetchStatus = 108,
  resHashMismatch = 109,
  resBuildResult = 110,
} result_type_t;

typedef uint64_t activity_id_t;

struct logger_settings_t : Config {
  setting_t<bool> showTrace{this, false, "show-trace",
                          R"(
          Whether Nix should print out a stack trace in case of Nix
          expression evaluation errors.
        )"};

  setting_t<std::optional<std::filesystem::path>> jsonLogPath{this,
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

extern logger_settings_t loggerSettings;

class Logger {
  friend struct activity_t;

public:
  struct field_t {
    // FIXME: use std::variant.
    enum { tInt = 0, tString = 1 } type;

    uint64_t i = 0;
    std::string s;

    field_t(const std::string& s) : type(tString), s(s) {}

    field_t(const char* s) : type(tString), s(s) {}

    field_t(const uint64_t& i) : type(tInt), i(i) {}
  };

  typedef std::vector<field_t> fields_t;

  virtual ~Logger() {}

  virtual void stop() {};

  /**
   * Guard object to resume the logger when done.
   */
  struct suspension_t {
    finally_t<std::function<void()>> _finalize;
  };

  suspension_t suspend();

  std::optional<suspension_t> suspendIf(bool cond);

  virtual void pause() {};
  virtual void resume() {};

  // Whether the logger prints the whole build log
  virtual bool isVerbose() { return false; }

  virtual void log(verbosity_t lvl, std::string_view s) = 0;

  void log(std::string_view s) { log(lvlInfo, s); }

  virtual void logEI(const error_info_t& ei) = 0;

  void logEI(verbosity_t lvl, error_info_t ei) {
    ei.level = lvl;
    logEI(ei);
  }

  virtual void warn(const std::string& msg);

  virtual void startActivity(activity_id_t act, verbosity_t lvl, activity_type_t type, const std::string& s,
                             const fields_t& fields, activity_id_t parent) {};

  virtual void stopActivity(activity_id_t act) {};

  virtual void result(activity_id_t act, result_type_t type, const fields_t& fields) {};

  virtual void result(activity_id_t act, result_type_t type, const nlohmann::json& json) {};

  virtual void writeToStdout(std::string_view s);

  template <typename... Args>
  inline void cout(const Args&... args) {
    writeToStdout(fmt(args...));
  }

  virtual std::optional<char> ask(std::string_view s) { return {}; }

  virtual void setPrintBuildLogs(bool printBuildLogs) {}
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

activity_id_t getCurActivity();
void setCurActivity(const activity_id_t activityId);

struct activity_t {
  Logger& logger;

  const activity_id_t id;

  activity_t(Logger& logger, verbosity_t lvl, activity_type_t type, const std::string& s = "",
           const Logger::fields_t& fields = {}, activity_id_t parent = getCurActivity());

  activity_t(Logger& logger, activity_type_t type, const Logger::fields_t& fields = {},
           activity_id_t parent = getCurActivity())
      : activity_t(logger, lvlError, type, "", fields, parent) {};

  activity_t(const activity_t& act) = delete;

  ~activity_t();

  void progress(uint64_t done = 0, uint64_t expected = 0, uint64_t running = 0,
                uint64_t failed = 0) const {
    result(resProgress, done, expected, running, failed);
  }

  void setExpected(activity_type_t type2, uint64_t expected) const {
    result(resSetExpected, type2, expected);
  }

  void result(result_type_t type, const nlohmann::json& json) const { logger.result(id, type, json); }

  template <typename... Args>
  void result(result_type_t type, const Args&... args) const {
    Logger::fields_t fields;
    nop{(fields.emplace_back(Logger::field_t(args)), 1)...};
    result(type, fields);
  }

  void result(result_type_t type, const Logger::fields_t& fields) const {
    logger.result(id, type, fields);
  }

  friend class Logger;
};

struct push_activity_t {
  const activity_id_t prevAct;

  push_activity_t(activity_id_t act) : prevAct(getCurActivity()) { setCurActivity(act); }

  ~push_activity_t() { setCurActivity(prevAct); }
};

extern std::unique_ptr<Logger> logger;

std::unique_ptr<Logger> makeSimpleLogger(bool printBuildLogs = true);

/**
 * Create a logger that sends log messages to `mainLogger` and the
 * list of loggers in `extraLoggers`. Only `mainLogger` is used for
 * writing to stdout and getting user input.
 */
std::unique_ptr<Logger> makeTeeLogger(std::unique_ptr<Logger> mainLogger,
                                      std::vector<std::unique_ptr<Logger>>&& extraLoggers);

std::unique_ptr<Logger> makeJSONLogger(descriptor_t fd, bool includeNixPrefix = true);

std::unique_ptr<Logger> makeJSONLogger(const std::filesystem::path& path,
                                       bool includeNixPrefix = true);

void applyJSONLogger();

/**
 * @param source A noun phrase describing the source of the message, e.g. "the builder".
 */
std::optional<nlohmann::json> parseJSONMessage(const std::string& msg, std::string_view source);

/**
 * @param source A noun phrase describing the source of the message, e.g. "the builder".
 */
bool handleJSONLogMessage(nlohmann::json& json, const activity_t& act,
                          std::map<activity_id_t, activity_t>& activities, std::string_view source,
                          bool trusted);

/**
 * @param source A noun phrase describing the source of the message, e.g. "the builder".
 */
bool handleJSONLogMessage(const std::string& msg, const activity_t& act,
                          std::map<activity_id_t, activity_t>& activities, std::string_view source,
                          bool trusted);

/**
 * suppress msgs > this
 */
extern verbosity_t verbosity;

/**
 * Print a message with the standard error_info_t format.
 * In general, use these 'log' macros for reporting problems that may require user
 * intervention or that need more explanation.  Use the 'print' macros for more
 * lightweight status messages.
 */
#define logErrorInfo(level, errorInfo...)                                                          \
  do {                                                                                             \
    if ((level) <= nix::verbosity) {                                                               \
      logger->logEI((level), errorInfo);                                                           \
    }                                                                                              \
  } while (0)

#define logError(errorInfo...) logErrorInfo(lvlError, errorInfo)
#define logWarning(errorInfo...) logErrorInfo(lvlWarn, errorInfo)

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

#define printError(args...) printMsg(lvlError, args)
#define notice(args...) printMsg(lvlNotice, args)
#define printInfo(args...) printMsg(lvlInfo, args)
#define printTalkative(args...) printMsg(lvlTalkative, args)
#define debug(args...) printMsg(lvlDebug, args)
#define vomit(args...) printMsg(lvlVomit, args)

/**
 * if verbosity >= lvlWarn, print a message with a yellow 'warning:' prefix.
 */
template <typename... Args>
inline void warn(const std::string& fs, const Args&... args) {
  boost::format f(fs);
  formatHelper(f, args...);
  logger->warn(f.str());
}

#define warnOnce(haveWarned, args...)                                                              \
  if (!haveWarned) {                                                                               \
    haveWarned = true;                                                                             \
    warn(args);                                                                                    \
  }

void writeToStderr(std::string_view s);

} // namespace nix
