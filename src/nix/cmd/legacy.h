#pragma once
///@file

#include <functional>
#include <map>
#include <string>

namespace nix {

using MainFunction = std::function<void(int, char**)>;

struct RegisterLegacyCommand {
  typedef std::map<std::string, MainFunction> commands_t;

  static commands_t& commands();

  RegisterLegacyCommand(const std::string& name, MainFunction fun) { commands()[name] = fun; }
};

} // namespace nix
