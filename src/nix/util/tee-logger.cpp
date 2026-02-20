#include "nix/util/logging.h"

namespace nix {

struct tee_logger_t : logger_t {
  std::vector<std::unique_ptr<logger_t>> loggers;

  tee_logger_t(std::vector<std::unique_ptr<logger_t>>&& loggers) : loggers(std::move(loggers)) {}

  void stop() override {
    for (auto& logger : loggers) {
      logger->stop();
    }
  };

  void pause() override {
    for (auto& logger : loggers) {
      logger->pause();
    }
  };

  void resume() override {
    for (auto& logger : loggers) {
      logger->resume();
    }
  };

  void log(verbosity_t lvl, std::string_view s) override {
    for (auto& logger : loggers) {
      logger->log(lvl, s);
    }
  }

  void log_ei(const error_info_t& ei) override {
    for (auto& logger : loggers) {
      logger->log_ei(ei);
    }
  }

  void start_activity(activity_id_t act, verbosity_t lvl, activity_type_t type,
                      const std::string& s, const fields_t& fields, activity_id_t parent) override {
    for (auto& logger : loggers) {
      logger->start_activity(act, lvl, type, s, fields, parent);
    }
  }

  void stop_activity(activity_id_t act) override {
    for (auto& logger : loggers) {
      logger->stop_activity(act);
    }
  }

  void result(activity_id_t act, result_type_t type, const fields_t& fields) override {
    for (auto& logger : loggers) {
      logger->result(act, type, fields);
    }
  }

  void result(activity_id_t act, result_type_t type, const nlohmann::json& json) override {
    for (auto& logger : loggers) {
      logger->result(act, type, json);
    }
  }

  void write_to_stdout(std::string_view s) override {
    for (auto& logger : loggers) {
      /* Let only the first logger write to stdout to avoid
         duplication. This means that the first logger needs to
         be the one managing stdout/stderr
         (e.g. `progress_bar_t`). */
      logger->write_to_stdout(s);
      break;
    }
  }

  std::optional<char> ask(std::string_view s) override {
    for (auto& logger : loggers) {
      auto c = logger->ask(s);
      if (c) {
        return c;
      }
    }
    return std::nullopt;
  }

  void set_print_build_logs(bool print_build_logs) override {
    for (auto& logger : loggers) {
      logger->set_print_build_logs(print_build_logs);
    }
  }
};

std::unique_ptr<logger_t> make_tee_logger(std::unique_ptr<logger_t> main_logger,
                                          std::vector<std::unique_ptr<logger_t>>&& extra_loggers) {
  std::vector<std::unique_ptr<logger_t>> all_loggers;
  all_loggers.push_back(std::move(main_logger));
  for (auto& l : extra_loggers) {
    all_loggers.push_back(std::move(l));
  }
  return std::make_unique<tee_logger_t>(std::move(all_loggers));
}

} // namespace nix
