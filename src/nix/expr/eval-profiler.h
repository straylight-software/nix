#pragma once
/**
 * @file
 *
 * Evaluation profiler interface definitions and builtin implementations.
 */

#include <bitset>
#include <filesystem>
#include <optional>
#include <span>
#include <vector>

#include "nix/util/ref.h"

namespace nix {

class eval_state_t;
class pos_idx_t;
struct value_t;

class EvalProfiler {
public:
  enum Hook {
    preFunctionCall,
    postFunctionCall,
  };

  static constexpr std::size_t numHooks = Hook::postFunctionCall + 1;
  using Hooks = std::bitset<numHooks>;

private:
  std::optional<Hooks> neededHooks;

protected:
  /** Invalidate the cached neededHooks. */
  void invalidateNeededHooks() { neededHooks = std::nullopt; }

  /**
   * Get which hooks need to be called.
   *
   * This is the actual implementation which has to be defined by subclasses.
   * Public API goes through the needsHooks, which is a
   * non-virtual interface (NVI) which caches the return value.
   */
  virtual Hooks getNeededHooksImpl() const { return Hooks{}; }

public:
  /**
   * Hook called in the eval_state_t::callFunction preamble.
   * Gets called only if (getNeededHooks().test(Hook::preFunctionCall)) is true.
   *
   * @param state Evaluator state.
   * @param v Function being invoked.
   * @param args Function arguments.
   * @param pos Function position.
   */
  virtual void pre_function_call_hook(eval_state_t& state, const value_t& v,
                                      std::span<value_t*> args, const pos_idx_t pos);

  /**
   * Hook called on eval_state_t::callFunction exit.
   * Gets called only if (getNeededHooks().test(Hook::postFunctionCall)) is true.
   *
   * @param state Evaluator state.
   * @param v Function being invoked.
   * @param args Function arguments.
   * @param pos Function position.
   */
  virtual void post_function_call_hook(eval_state_t& state, const value_t& v,
                                       std::span<value_t*> args, const pos_idx_t pos);

  virtual ~EvalProfiler() = default;

  /**
   * Get which hooks need to be invoked for this EvalProfiler instance.
   */
  Hooks getNeededHooks() {
    if (neededHooks.has_value()) {
      return *neededHooks;
    }
    return *(neededHooks = getNeededHooksImpl());
  }
};

/**
 * Profiler that invokes multiple profilers at once.
 */
class MultiEvalProfiler : public EvalProfiler {
  std::vector<ref<EvalProfiler>> profilers;

  [[gnu::noinline]] Hooks getNeededHooksImpl() const override;

public:
  MultiEvalProfiler() = default;

  /** Register a profiler instance. */
  void addProfiler(ref<EvalProfiler> profiler);

  [[gnu::noinline]] void pre_function_call_hook(eval_state_t& state, const value_t& v,
                                                std::span<value_t*> args,
                                                const pos_idx_t pos) override;
  [[gnu::noinline]] void post_function_call_hook(eval_state_t& state, const value_t& v,
                                                 std::span<value_t*> args,
                                                 const pos_idx_t pos) override;
};

ref<EvalProfiler> make_sample_stack_profiler(eval_state_t& state,
                                             std::filesystem::path profile_file,
                                             uint64_t frequency);

} // namespace nix
