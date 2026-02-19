#pragma once
///@file

#include <map>
#include <string>

#include <nlohmann/json_fwd.hpp>

#include "nix/expr/eval.h"
#include "nix/expr/nixexpr.h"

namespace nix {

nlohmann::json print_value_as_json(EvalState& state, bool strict, Value& v, const pos_idx_t pos,
                                NixStringContext& context, bool copy_to_store = true);

void print_value_as_json(EvalState& state, bool strict, Value& v, const pos_idx_t pos, std::ostream& str,
                      NixStringContext& context, bool copy_to_store = true);

make_error(JSONSerializationError, Error);

} // namespace nix
