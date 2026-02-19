#pragma once
///@file

#include <string>

#include "nix/util/error.h"

namespace nix {

class EvalState;
struct Value;

make_error(JSONParseError, Error);

void parse_json(EvalState& state, const std::string_view& s, Value& v);

} // namespace nix
