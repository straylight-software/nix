#pragma once
///@file

#include <string>

#include "nix/util/error.h"

namespace nix {

class EvalState;
struct Value;

MakeError(JSONParseError, Error);

void parseJSON(EvalState& state, const std::string_view& s, Value& v);

} // namespace nix
