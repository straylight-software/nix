#pragma once
///@file

#include <map>
#include <string>

#include "nix/expr/eval.h"
#include "nix/expr/nixexpr.h"

namespace nix {

void printValueAsXML(EvalState& state, bool strict, bool location, Value& v, std::ostream& out,
                     NixStringContext& context, const PosIdx pos);

}
