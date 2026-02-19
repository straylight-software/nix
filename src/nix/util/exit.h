#pragma once

#include <exception>

namespace nix {

/**
 * exit_t the program with a given exit code.
 */
class [[nodiscard]] exit_t : public std::exception {
public:
  exit_t() : status_(0) {}

  explicit exit_t(int status) : status_(status) {}

  ~exit_t() override;

  [[nodiscard]] auto get_status() const -> int { return status_; }

private:
  int status_;
};

} // namespace nix
