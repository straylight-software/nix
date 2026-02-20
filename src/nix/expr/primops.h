#pragma once
///@file

#include <tuple>
#include <vector>

#include "nix/expr/eval.h"

namespace nix {

struct RegisterPrimOp {
  typedef std::vector<PrimOp> PrimOps;

  static PrimOps& primOps();

  /**
   * You can register a constant by passing an arity of 0. fun
   * will get called during eval_state_t initialization, so there
   * may be primops not yet added and builtins is not yet sorted.
   */
  RegisterPrimOp(PrimOp&& prim_op);
};

/* These primops are disabled without enableNativeCode, but plugins
   may wish to use them in limited contexts without globally enabling
   them. */

/**
 * Load a value_initializer_t from a DSO and return whatever it initializes
 */
void prim_import_native(eval_state_t& state, const pos_idx_t pos, value_t** args, value_t& v);

/**
 * Execute a program and parse its output
 */
void prim_exec(eval_state_t& state, const pos_idx_t pos, value_t** args, value_t& v);

void make_position_thunks(eval_state_t& state, const pos_idx_t pos, value_t& line, value_t& column);

} // namespace nix
