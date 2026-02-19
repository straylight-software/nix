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

logger_settings_t logger_settings;

static global_config_t::Register r_logger_settings(&logger_settings);

static thread_local activity_id_t cur_activity = 0;

activity_id_t get_cur_activity() {
  return cur_activity;
}

void set_cur_activity(const activity_id_t activity_id) {
  cur_activity = activity_id;
}

std::unique_ptr<logger_t> logger = make_simple_logger(true);

void logger_t::warn(const std::string& msg) {
  log(lvl_warn, ANSI_WARNING "warning:" ANSI_NORMAL " " + msg);
}

void logger_t::write_to_stdout(std::string_view s) {
  descriptor_t standard_out = get_standard_output();
  write_full(standard_out, s);
  write_full(standard_out, "\n");
}

logger_t::suspension_t logger_t::suspend() {
  pause();
  return suspension_t{._finalize = {[this]() { this->resume(); }}};
}

std::optional<logger_t::suspension_t> logger_t::suspend_if(bool cond) {
  if (cond)
    return suspend();
  return {};
}

class simple_logger_t : public logger_t {
public:
  bool systemd, tty;
  bool print_build_logs;

  simple_logger_t(bool print_build_logs) : print_build_logs(print_build_logs) {
    systemd = get_env("IN_SYSTEMD") == "1";
    tty = is_tty();
  }

  bool is_verbose() override { return print_build_logs; }

  void log(verbosity_t lvl, std::string_view s) override {
    if (lvl > verbosity)
      return;

    std::string prefix;

    if (systemd) {
      char c;
      switch (lvl) {
        case lvl_error:
          c = '3';
          break;
        case lvl_warn:
          c = '4';
          break;
        case lvl_notice:
        case lvl_info:
          c = '5';
          break;
        case lvl_talkative:
        case lvl_chatty:
          c = '6';
          break;
        case lvl_debug:
        case lvl_vomit:
          c = '7';
          break;
        default:
          c = '7';
          break; // should not happen, and missing enum case is reported by -Werror=switch-enum
      }
      prefix = std::string("<") + c + ">";
    }

    write_to_stderr(prefix + filter_ansi_escapes(s, !tty) + "\n");
  }

  void log_ei(const error_info_t& ei) override {
    std::ostringstream oss;
    show_error_info(oss, ei, logger_settings.show_trace.get());

    log(ei.level, oss.view());
  }

  void start_activity(activity_id_t act, verbosity_t lvl, activity_type_t type, const std::string& s,
                     const fields_t& fields, activity_id_t parent) override {
    if (lvl <= verbosity && !s.empty())
      log(lvl, s + "...");
  }

  void result(activity_id_t act, result_type_t type, const fields_t& fields) override {
    if (type == res_build_log_line && print_build_logs) {
      auto last_line = fields[0].s;
      printError(last_line);
    } else if (type == res_post_build_log_line && print_build_logs) {
      auto last_line = fields[0].s;
      printError("post-build-hook: " + last_line);
    }
  }
};

verbosity_t verbosity = lvl_info;

void write_to_stderr(std::string_view s) {
  try {
    write_full(get_standard_error(), s, false);
  } catch (SystemError& e) {
    /* Ignore failing writes to stderr.  We need to ignore write
       errors to ensure that cleanup code that logs to stderr runs
       to completion if the other side of stderr has been closed
       unexpectedly. */
  }
}

std::unique_ptr<logger_t> make_simple_logger(bool print_build_logs) {
  return std::make_unique<simple_logger_t>(print_build_logs);
}

std::atomic<uint64_t> next_id{0};

static uint64_t get_pid() {
#ifndef _WIN32
  return getpid();
#else
  return GetCurrentProcessId();
#endif
}

activity_t::activity_t(logger_t& logger, verbosity_t lvl, activity_type_t type, const std::string& s,
                   const logger_t::fields_t& fields, activity_id_t parent)
    : logger(logger), id(next_id++ + (((uint64_t)get_pid()) << 32)) {
  logger.start_activity(id, lvl, type, s, fields, parent);
}

void to_json(nlohmann::json& json, std::shared_ptr<const pos_t> pos) {
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

struct json_logger_t : logger_t {
  descriptor_t fd;
  bool include_nix_prefix;

  json_logger_t(descriptor_t fd, bool include_nix_prefix) : fd(fd), include_nix_prefix(include_nix_prefix) {}

  bool is_verbose() override { return true; }

  void add_fields(nlohmann::json& json, const fields_t& fields) {
    if (fields.empty())
      return;
    auto& arr = json["fields"] = nlohmann::json::array();
    for (auto& f : fields)
      if (f.type == logger_t::field_t::t_int)
        arr.push_back(f.i);
      else if (f.type == logger_t::field_t::t_string)
        arr.push_back(f.s);
      else
        unreachable();
  }

  struct State {
    bool enabled = true;
  };

  sync_t<State> _state;

  void write(const nlohmann::json& json) {
    auto line = (include_nix_prefix ? "@nix " : "") +
                json.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);

    /* Acquire a lock to prevent log messages from clobbering each
       other. */
    try {
      auto state(_state.lock());
      if (state->enabled)
        write_line(fd, line);
    } catch (...) {
      bool enabled = false;
      std::swap(_state.lock()->enabled, enabled);
      if (enabled) {
        ignore_exception_except_interrupt();
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

  void log_ei(const error_info_t& ei) override {
    std::ostringstream oss;
    show_error_info(oss, ei, logger_settings.show_trace.get());

    nlohmann::json json;
    json["action"] = "msg";
    json["level"] = ei.level;
    json["msg"] = oss.str();
    json["raw_msg"] = ei.msg.str();
    to_json(json, ei.pos);

    if (logger_settings.show_trace.get() && !ei.traces.empty()) {
      nlohmann::json traces = nlohmann::json::array();
      for (auto iter = ei.traces.rbegin(); iter != ei.traces.rend(); ++iter) {
        nlohmann::json stack_frame;
        stack_frame["raw_msg"] = iter->hint.str();
        to_json(stack_frame, iter->pos);
        traces.push_back(stack_frame);
      }

      json["trace"] = traces;
    }

    write(json);
  }

  void start_activity(activity_id_t act, verbosity_t lvl, activity_type_t type, const std::string& s,
                     const fields_t& fields, activity_id_t parent) override {
    nlohmann::json json;
    json["action"] = "start";
    json["id"] = act;
    json["level"] = lvl;
    json["type"] = type;
    json["text"] = s;
    json["parent"] = parent;
    add_fields(json, fields);
    write(json);
  }

  void stop_activity(activity_id_t act) override {
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
    add_fields(json, fields);
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

std::unique_ptr<logger_t> make_json_logger(descriptor_t fd, bool include_nix_prefix) {
  return std::make_unique<json_logger_t>(fd, include_nix_prefix);
}

std::unique_ptr<logger_t> make_json_logger(const std::filesystem::path& path, bool include_nix_prefix) {
  struct json_file_logger_t : json_logger_t {
    auto_close_fd_t fd;

    json_file_logger_t(auto_close_fd_t&& fd, bool include_nix_prefix)
        : json_logger_t(fd.get(), include_nix_prefix), fd(std::move(fd)) {}
  };

  auto_close_fd_t fd =
      std::filesystem::is_socket(path)
          ? connect(path)
          : to_descriptor(open(path.string().c_str(), O_CREAT | O_APPEND | O_WRONLY, 0644));
  if (!fd)
    throw sys_error_t("opening log file %1%", path);

  return std::make_unique<json_file_logger_t>(std::move(fd), include_nix_prefix);
}

void apply_json_logger() {
  if (auto& opt = logger_settings.json_log_path.get()) {
    try {
      std::vector<std::unique_ptr<logger_t>> loggers;
      loggers.push_back(make_json_logger(*opt, false));
      try {
        logger = make_tee_logger(std::move(logger), std::move(loggers));
      } catch (...) {
        // `logger` is now gone so give up.
        abort();
      }
    } catch (...) {
      ignore_exception_except_interrupt();
    }
  }
}

static logger_t::fields_t get_fields(nlohmann::json& json) {
  logger_t::fields_t fields;
  for (auto& f : json) {
    if (f.type() == nlohmann::json::value_t::number_unsigned)
      fields.emplace_back(logger_t::field_t(f.get<uint64_t>()));
    else if (f.type() == nlohmann::json::value_t::string)
      fields.emplace_back(logger_t::field_t(f.get<std::string>()));
    else
      throw Error("unsupported JSON type %d", (int)f.type());
  }
  return fields;
}

std::optional<nlohmann::json> parse_json_message(const std::string& msg, std::string_view source) {
  if (!has_prefix(msg, "@nix "))
    return std::nullopt;
  try {
    return nlohmann::json::parse(std::string(msg, 5));
  } catch (std::exception& e) {
    printError("bad JSON log message from %s: %s", uncolored_t(source), e.what());
  }
  return std::nullopt;
}

bool handle_json_log_message(nlohmann::json& json, const activity_t& act,
                          std::map<activity_id_t, activity_t>& activities, std::string_view source,
                          bool trusted) {
  try {
    std::string action = json["action"];

    if (action == "start") {
      auto type = (activity_type_t)json["type"];
      if (trusted || type == act_file_transfer)
        activities.emplace(std::piecewise_construct, std::forward_as_tuple(json["id"]),
                           std::forward_as_tuple(*logger, (verbosity_t)json["level"], type,
                                                 json["text"], get_fields(json["fields"]), act.id));
    }

    else if (action == "stop")
      activities.erase((activity_id_t)json["id"]);

    else if (action == "result") {
      auto i = activities.find((activity_id_t)json["id"]);
      if (i != activities.end())
        i->second.result((result_type_t)json["type"], get_fields(json["fields"]));
    }

    else if (action == "setPhase") {
      std::string phase = json["phase"];
      act.result(res_set_phase, phase);
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

bool handle_json_log_message(const std::string& msg, const activity_t& act,
                          std::map<activity_id_t, activity_t>& activities, std::string_view source,
                          bool trusted) {
  auto json = parse_json_message(msg, source);
  if (!json)
    return false;

  return handle_json_log_message(*json, act, activities, source, trusted);
}

activity_t::~activity_t() {
  try {
    logger.stop_activity(id);
  } catch (...) {
    ignore_exception_in_destructor();
  }
}

} // namespace nix
