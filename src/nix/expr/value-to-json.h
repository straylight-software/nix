#pragma once
///@file

#include <map>
#include <string>

#include <nlohmann/json_fwd.hpp>

#include "nix/expr/eval.h"
#include "nix/expr/nixexpr.h"

namespace nix {

nlohmann::json printValueAsJSON(EvalState& state, bool strict, Value& v, const pos_idx_t pos,
                                NixStringContext& context, bool copyToStore = true);

void printValueAsJSON(EvalState& state, bool strict, Value& v, const pos_idx_t pos, std::ostream& str,
                      NixStringContext& context, bool copyToStore = true);

MakeError(JSONSerializationError, Error);

} // namespace nix
