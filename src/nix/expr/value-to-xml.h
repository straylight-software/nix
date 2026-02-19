#pragma once
///@file

#include <map>
#include <string>

#include "nix/expr/eval.h"
#include "nix/expr/nixexpr.h"

namespace nix {

void print_value_as_xml(EvalState& state, bool strict, bool location, Value& v, std::ostream& out,
                     NixStringContext& context, const pos_idx_t pos);

}
