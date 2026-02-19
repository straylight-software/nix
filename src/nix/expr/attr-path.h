#pragma once
///@file

#include <map>
#include <string>

#include "nix/expr/eval.h"

namespace nix {

make_error(AttrPathNotFound, Error);
make_error(NoPositionInfo, Error);

std::pair<Value*, pos_idx_t> find_along_attr_path(EvalState& state, const std::string& attr_path,
                                            Bindings& auto_args, Value& v_in);

/**
 * Heuristic to find the filename and lineno or a nix value.
 */
std::pair<source_path_t, uint32_t> find_package_filename(EvalState& state, Value& v, std::string what);

struct AttrPath : std::vector<Symbol> {
  using std::vector<Symbol>::vector;

  static AttrPath parse(EvalState& state, std::string_view s);

  std::string to_string(EvalState& state) const;

  std::vector<SymbolStr> resolve(EvalState& state) const;
};

} // namespace nix
