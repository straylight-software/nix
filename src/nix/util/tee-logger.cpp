#include "nix/util/logging.h"

namespace nix {

struct tee_logger_t : Logger {
  std::vector<std::unique_ptr<Logger>> loggers;

  tee_logger_t(std::vector<std::unique_ptr<Logger>>&& loggers) : loggers(std::move(loggers)) {}

  void stop() override {
    for (auto& logger : loggers)
      logger->stop();
  };

  void pause() override {
    for (auto& logger : loggers)
      logger->pause();
  };

  void resume() override {
    for (auto& logger : loggers)
      logger->resume();
  };

  void log(verbosity_t lvl, std::string_view s) override {
    for (auto& logger : loggers)
      logger->log(lvl, s);
  }

  void logEI(const error_info_t& ei) override {
    for (auto& logger : loggers)
      logger->logEI(ei);
  }

  void startActivity(activity_id_t act, verbosity_t lvl, activity_type_t type, const std::string& s,
                     const fields_t& fields, activity_id_t parent) override {
    for (auto& logger : loggers)
      logger->startActivity(act, lvl, type, s, fields, parent);
  }

  void stopActivity(activity_id_t act) override {
    for (auto& logger : loggers)
      logger->stopActivity(act);
  }

  void result(activity_id_t act, result_type_t type, const fields_t& fields) override {
    for (auto& logger : loggers)
      logger->result(act, type, fields);
  }

  void result(activity_id_t act, result_type_t type, const nlohmann::json& json) override {
    for (auto& logger : loggers)
      logger->result(act, type, json);
  }

  void writeToStdout(std::string_view s) override {
    for (auto& logger : loggers) {
      /* Let only the first logger write to stdout to avoid
         duplication. This means that the first logger needs to
         be the one managing stdout/stderr
         (e.g. `progress_bar_t`). */
      logger->writeToStdout(s);
      break;
    }
  }

  std::optional<char> ask(std::string_view s) override {
    for (auto& logger : loggers) {
      auto c = logger->ask(s);
      if (c)
        return c;
    }
    return std::nullopt;
  }

  void setPrintBuildLogs(bool printBuildLogs) override {
    for (auto& logger : loggers)
      logger->setPrintBuildLogs(printBuildLogs);
  }
};

std::unique_ptr<Logger> makeTeeLogger(std::unique_ptr<Logger> mainLogger,
                                      std::vector<std::unique_ptr<Logger>>&& extraLoggers) {
  std::vector<std::unique_ptr<Logger>> allLoggers;
  allLoggers.push_back(std::move(mainLogger));
  for (auto& l : extraLoggers)
    allLoggers.push_back(std::move(l));
  return std::make_unique<tee_logger_t>(std::move(allLoggers));
}

} // namespace nix
