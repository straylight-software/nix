#include "nix/expr/function-trace.h"

#include "nix/util/logging.h"

namespace nix {

void FunctionCallTrace::pre_function_call_hook(eval_state_t& state, const value_t& v,
                                               std::span<value_t*> args, const pos_idx_t pos) {
  auto duration = std::chrono::high_resolution_clock::now().time_since_epoch();
  auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(duration);
  printMsg(lvl_info, "function-trace entered %1% at %2%", state.positions[pos], ns.count());
}

void FunctionCallTrace::post_function_call_hook(eval_state_t& state, const value_t& v,
                                                std::span<value_t*> args, const pos_idx_t pos) {
  auto duration = std::chrono::high_resolution_clock::now().time_since_epoch();
  auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(duration);
  printMsg(lvl_info, "function-trace exited %1% at %2%", state.positions[pos], ns.count());
}

} // namespace nix
