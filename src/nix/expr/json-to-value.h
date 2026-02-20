#pragma once
///@file

#include <string>

#include "nix/util/error.h"

namespace nix {

class eval_state_t;
struct value_t;

make_error(JSONParseError, Error);

void parse_json(eval_state_t& state, const std::string_view& s, value_t& v);

} // namespace nix
