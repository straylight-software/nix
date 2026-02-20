#pragma once
///@file

#include <map>
#include <string>

#include "nix/expr/eval.h"

namespace nix {

make_error(AttrPathNotFound, Error);
make_error(NoPositionInfo, Error);

std::pair<value_t*, pos_idx_t> find_along_attr_path(eval_state_t& state, const std::string& attr_path,
                                            bindings_t& auto_args, value_t& v_in);

/**
 * Heuristic to find the filename and lineno or a nix value.
 */
std::pair<source_path_t, uint32_t> find_package_filename(eval_state_t& state, value_t& v, std::string what);

struct AttrPath : std::vector<symbol_t> {
  using std::vector<symbol_t>::vector;

  static AttrPath parse(eval_state_t& state, std::string_view s);

  std::string to_string(eval_state_t& state) const;

  std::vector<SymbolStr> resolve(eval_state_t& state) const;
};

} // namespace nix
