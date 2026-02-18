#pragma once
///@file

#include <map>
#include <string>

#include <nlohmann/json_fwd.hpp>

#include "nix/expr/eval.h"
#include "nix/expr/nixexpr.h"

namespace nix {

nlohmann::json printValueAsJSON(EvalState& state, bool strict, Value& v, const PosIdx pos,
                                NixStringContext& context, bool copyToStore = true);

void printValueAsJSON(EvalState& state, bool strict, Value& v, const PosIdx pos, std::ostream& str,
                      NixStringContext& context, bool copyToStore = true);

MakeError(JSONSerializationError, Error);

} // namespace nix
