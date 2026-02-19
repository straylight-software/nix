#pragma once

#include <exception>

namespace nix {

/**
 * exit_t the program with a given exit code.
 */
class exit_t : public std::exception {
public:
  int status;

  exit_t() : status(0) {}

  explicit exit_t(int status) : status(status) {}

  virtual ~exit_t();
};

} // namespace nix
