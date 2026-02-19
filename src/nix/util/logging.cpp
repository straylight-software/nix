#include "nix/util/logging.h"

#include <atomic>
#include <iostream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "nix/util/config-global.h"
#include "nix/util/environment-variables.h"
#include "nix/util/file-descriptor.h"
#include "nix/util/position.h"
#include "nix/util/source-path.h"
#include "nix/util/sync.h"
#include "nix/util/terminal.h"
#include "nix/util/unix-domain-socket.h"
#include "nix/util/util.h"

namespace nix {

logger_settings_t loggerSettings;

static global_config_t::Register rLoggerSettings(&loggerSettings);

static thread_local activity_id_t curActivity = 0;

activity_id_t getCurActivity() {
  return curActivity;
}

void setCurActivity(const activity_id_t activityId) {
  curActivity = activityId;
}

std::unique_ptr<Logger> logger = makeSimpleLogger(true);

void Logger::warn(const std::string& msg) {
  log(lvlWarn, ANSI_WARNING "warning:" ANSI_NORMAL " " + msg);
}

void Logger::writeToStdout(std::string_view s) {
  descriptor_t standard_out = getStandardOutput();
  writeFull(standard_out, s);
  writeFull(standard_out, "\n");
}

Logger::suspension_t Logger::suspend() {
  pause();
  return suspension_t{._finalize = {[this]() { this->resume(); }}};
}

std::optional<Logger::suspension_t> Logger::suspendIf(bool cond) {
  if (cond)
    return suspend();
  return {};
}

class simple_logger_t : public Logger {
public:
  bool systemd, tty;
  bool printBuildLogs;

  simple_logger_t(bool printBuildLogs) : printBuildLogs(printBuildLogs) {
    systemd = getEnv("IN_SYSTEMD") == "1";
    tty = isTTY();
  }

  bool isVerbose() override { return printBuildLogs; }

  void log(verbosity_t lvl, std::string_view s) override {
    if (lvl > verbosity)
      return;

    std::string prefix;

    if (systemd) {
      char c;
      switch (lvl) {
        case lvlError:
          c = '3';
          break;
        case lvlWarn:
          c = '4';
          break;
        case lvlNotice:
        case lvlInfo:
          c = '5';
          break;
        case lvlTalkative:
        case lvlChatty:
          c = '6';
          break;
        case lvlDebug:
        case lvlVomit:
          c = '7';
          break;
        default:
          c = '7';
          break; // should not happen, and missing enum case is reported by -Werror=switch-enum
      }
      prefix = std::string("<") + c + ">";
    }

    writeToStderr(prefix + filterANSIEscapes(s, !tty) + "\n");
  }

  void logEI(const error_info_t& ei) override {
    std::ostringstream oss;
    showErrorInfo(oss, ei, loggerSettings.showTrace.get());

    log(ei.level, oss.view());
  }

  void startActivity(activity_id_t act, verbosity_t lvl, activity_type_t type, const std::string& s,
                     const fields_t& fields, activity_id_t parent) override {
    if (lvl <= verbosity && !s.empty())
      log(lvl, s + "...");
  }

  void result(activity_id_t act, result_type_t type, const fields_t& fields) override {
    if (type == resBuildLogLine && printBuildLogs) {
      auto lastLine = fields[0].s;
      printError(lastLine);
    } else if (type == resPostBuildLogLine && printBuildLogs) {
      auto lastLine = fields[0].s;
      printError("post-build-hook: " + lastLine);
    }
  }
};

verbosity_t verbosity = lvlInfo;

void writeToStderr(std::string_view s) {
  try {
    writeFull(getStandardError(), s, false);
  } catch (SystemError& e) {
    /* Ignore failing writes to stderr.  We need to ignore write
       errors to ensure that cleanup code that logs to stderr runs
       to completion if the other side of stderr has been closed
       unexpectedly. */
  }
}

std::unique_ptr<Logger> makeSimpleLogger(bool printBuildLogs) {
  return std::make_unique<simple_logger_t>(printBuildLogs);
}

std::atomic<uint64_t> nextId{0};

static uint64_t getPid() {
#ifndef _WIN32
  return getpid();
#else
  return GetCurrentProcessId();
#endif
}

activity_t::activity_t(Logger& logger, verbosity_t lvl, activity_type_t type, const std::string& s,
                   const Logger::fields_t& fields, activity_id_t parent)
    : logger(logger), id(nextId++ + (((uint64_t)getPid()) << 32)) {
  logger.startActivity(id, lvl, type, s, fields, parent);
}

void to_json(nlohmann::json& json, std::shared_ptr<const Pos> pos) {
  if (pos) {
    json["line"] = pos->line;
    json["column"] = pos->column;
    std::ostringstream str;
    pos->print(str, true);
    json["file"] = str.str();
  } else {
    json["line"] = nullptr;
    json["column"] = nullptr;
    json["file"] = nullptr;
  }
}

struct json_logger_t : Logger {
  descriptor_t fd;
  bool includeNixPrefix;

  json_logger_t(descriptor_t fd, bool includeNixPrefix) : fd(fd), includeNixPrefix(includeNixPrefix) {}

  bool isVerbose() override { return true; }

  void addFields(nlohmann::json& json, const fields_t& fields) {
    if (fields.empty())
      return;
    auto& arr = json["fields"] = nlohmann::json::array();
    for (auto& f : fields)
      if (f.type == Logger::field_t::tInt)
        arr.push_back(f.i);
      else if (f.type == Logger::field_t::tString)
        arr.push_back(f.s);
      else
        unreachable();
  }

  struct State {
    bool enabled = true;
  };

  sync_t<State> _state;

  void write(const nlohmann::json& json) {
    auto line = (includeNixPrefix ? "@nix " : "") +
                json.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);

    /* Acquire a lock to prevent log messages from clobbering each
       other. */
    try {
      auto state(_state.lock());
      if (state->enabled)
        writeLine(fd, line);
    } catch (...) {
      bool enabled = false;
      std::swap(_state.lock()->enabled, enabled);
      if (enabled) {
        ignoreExceptionExceptInterrupt();
        logger->warn("disabling JSON logger due to write errors");
      }
    }
  }

  void log(verbosity_t lvl, std::string_view s) override {
    nlohmann::json json;
    json["action"] = "msg";
    json["level"] = lvl;
    json["msg"] = s;
    write(json);
  }

  void logEI(const error_info_t& ei) override {
    std::ostringstream oss;
    showErrorInfo(oss, ei, loggerSettings.showTrace.get());

    nlohmann::json json;
    json["action"] = "msg";
    json["level"] = ei.level;
    json["msg"] = oss.str();
    json["raw_msg"] = ei.msg.str();
    to_json(json, ei.pos);

    if (loggerSettings.showTrace.get() && !ei.traces.empty()) {
      nlohmann::json traces = nlohmann::json::array();
      for (auto iter = ei.traces.rbegin(); iter != ei.traces.rend(); ++iter) {
        nlohmann::json stackFrame;
        stackFrame["raw_msg"] = iter->hint.str();
        to_json(stackFrame, iter->pos);
        traces.push_back(stackFrame);
      }

      json["trace"] = traces;
    }

    write(json);
  }

  void startActivity(activity_id_t act, verbosity_t lvl, activity_type_t type, const std::string& s,
                     const fields_t& fields, activity_id_t parent) override {
    nlohmann::json json;
    json["action"] = "start";
    json["id"] = act;
    json["level"] = lvl;
    json["type"] = type;
    json["text"] = s;
    json["parent"] = parent;
    addFields(json, fields);
    write(json);
  }

  void stopActivity(activity_id_t act) override {
    nlohmann::json json;
    json["action"] = "stop";
    json["id"] = act;
    write(json);
  }

  void result(activity_id_t act, result_type_t type, const fields_t& fields) override {
    nlohmann::json json;
    json["action"] = "result";
    json["id"] = act;
    json["type"] = type;
    addFields(json, fields);
    write(json);
  }

  void result(activity_id_t act, result_type_t type, const nlohmann::json& j) override {
    nlohmann::json json;
    json["action"] = "result";
    json["id"] = act;
    json["type"] = type;
    json["payload"] = j;
    write(json);
  }
};

std::unique_ptr<Logger> makeJSONLogger(descriptor_t fd, bool includeNixPrefix) {
  return std::make_unique<json_logger_t>(fd, includeNixPrefix);
}

std::unique_ptr<Logger> makeJSONLogger(const std::filesystem::path& path, bool includeNixPrefix) {
  struct json_file_logger_t : json_logger_t {
    auto_close_fd_t fd;

    json_file_logger_t(auto_close_fd_t&& fd, bool includeNixPrefix)
        : json_logger_t(fd.get(), includeNixPrefix), fd(std::move(fd)) {}
  };

  auto_close_fd_t fd =
      std::filesystem::is_socket(path)
          ? connect(path)
          : toDescriptor(open(path.string().c_str(), O_CREAT | O_APPEND | O_WRONLY, 0644));
  if (!fd)
    throw sys_error_t("opening log file %1%", path);

  return std::make_unique<json_file_logger_t>(std::move(fd), includeNixPrefix);
}

void applyJSONLogger() {
  if (auto& opt = loggerSettings.jsonLogPath.get()) {
    try {
      std::vector<std::unique_ptr<Logger>> loggers;
      loggers.push_back(makeJSONLogger(*opt, false));
      try {
        logger = makeTeeLogger(std::move(logger), std::move(loggers));
      } catch (...) {
        // `logger` is now gone so give up.
        abort();
      }
    } catch (...) {
      ignoreExceptionExceptInterrupt();
    }
  }
}

static Logger::fields_t getFields(nlohmann::json& json) {
  Logger::fields_t fields;
  for (auto& f : json) {
    if (f.type() == nlohmann::json::value_t::number_unsigned)
      fields.emplace_back(Logger::field_t(f.get<uint64_t>()));
    else if (f.type() == nlohmann::json::value_t::string)
      fields.emplace_back(Logger::field_t(f.get<std::string>()));
    else
      throw Error("unsupported JSON type %d", (int)f.type());
  }
  return fields;
}

std::optional<nlohmann::json> parseJSONMessage(const std::string& msg, std::string_view source) {
  if (!hasPrefix(msg, "@nix "))
    return std::nullopt;
  try {
    return nlohmann::json::parse(std::string(msg, 5));
  } catch (std::exception& e) {
    printError("bad JSON log message from %s: %s", uncolored_t(source), e.what());
  }
  return std::nullopt;
}

bool handleJSONLogMessage(nlohmann::json& json, const activity_t& act,
                          std::map<activity_id_t, activity_t>& activities, std::string_view source,
                          bool trusted) {
  try {
    std::string action = json["action"];

    if (action == "start") {
      auto type = (activity_type_t)json["type"];
      if (trusted || type == actFileTransfer)
        activities.emplace(std::piecewise_construct, std::forward_as_tuple(json["id"]),
                           std::forward_as_tuple(*logger, (verbosity_t)json["level"], type,
                                                 json["text"], getFields(json["fields"]), act.id));
    }

    else if (action == "stop")
      activities.erase((activity_id_t)json["id"]);

    else if (action == "result") {
      auto i = activities.find((activity_id_t)json["id"]);
      if (i != activities.end())
        i->second.result((result_type_t)json["type"], getFields(json["fields"]));
    }

    else if (action == "setPhase") {
      std::string phase = json["phase"];
      act.result(resSetPhase, phase);
    }

    else if (action == "msg") {
      std::string msg = json["msg"];
      logger->log((verbosity_t)json["level"], msg);
    }

    return true;
  } catch (const nlohmann::json::exception& e) {
    warn("Unable to handle a JSON message from %s: %s", uncolored_t(source), e.what());
    return false;
  }
}

bool handleJSONLogMessage(const std::string& msg, const activity_t& act,
                          std::map<activity_id_t, activity_t>& activities, std::string_view source,
                          bool trusted) {
  auto json = parseJSONMessage(msg, source);
  if (!json)
    return false;

  return handleJSONLogMessage(*json, act, activities, source, trusted);
}

activity_t::~activity_t() {
  try {
    logger.stopActivity(id);
  } catch (...) {
    ignoreExceptionInDestructor();
  }
}

} // namespace nix
