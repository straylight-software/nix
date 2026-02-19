#pragma once
///@file

#include "nix/expr/eval-profiler.h"
#include "nix/expr/eval.h"

namespace nix {

class FunctionCallTrace : public EvalProfiler {
  Hooks getNeededHooksImpl() const override {
    return Hooks().set(preFunctionCall).set(postFunctionCall);
  }

public:
  FunctionCallTrace() = default;

  [[gnu::noinline]] void pre_function_call_hook(EvalState& state, const Value& v,
                                             std::span<Value*> args, const pos_idx_t pos) override;
  [[gnu::noinline]] void post_function_call_hook(EvalState& state, const Value& v,
                                              std::span<Value*> args, const pos_idx_t pos) override;
};

} // namespace nix
