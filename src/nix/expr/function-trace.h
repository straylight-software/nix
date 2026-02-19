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

  [[gnu::noinline]] void preFunctionCallHook(EvalState& state, const Value& v,
                                             std::span<Value*> args, const PosIdx pos) override;
  [[gnu::noinline]] void postFunctionCallHook(EvalState& state, const Value& v,
                                              std::span<Value*> args, const PosIdx pos) override;
};

} // namespace nix
