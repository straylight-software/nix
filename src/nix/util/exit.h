#ifndef NIX_UTIL_EXIT_H
#define NIX_UTIL_EXIT_H

#include <exception>

namespace nix {

/**
 * exit_t the program with a given exit code.
 */
class [[nodiscard]] exit_t : public std::exception {
public:
  exit_t() : status_(0) {}

  explicit exit_t(int status) : status_(status) {}

  ~exit_t() override = default;

  // Rule of 5: explicitly default copy/move operations
  exit_t(const exit_t&) = default;
  auto operator=(const exit_t&) -> exit_t& = default;
  exit_t(exit_t&&) noexcept = default;
  auto operator=(exit_t&&) noexcept -> exit_t& = default;

  [[nodiscard]] auto get_status() const -> int { return status_; }

private:
  int status_;
};

} // namespace nix

#endif // NIX_UTIL_EXIT_H
