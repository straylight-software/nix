#include "nix/expr/eval-profiler.h"

#include <fcntl.h>

#include "straylight/nix/data/lru_cache.h"

#include "nix/expr/eval.h"
#include "nix/expr/nixexpr.h"

namespace nix {

void EvalProfiler::pre_function_call_hook(eval_state_t& state, const value_t& v,
                                          std::span<value_t*> args, const pos_idx_t pos) {}

void EvalProfiler::post_function_call_hook(eval_state_t& state, const value_t& v,
                                           std::span<value_t*> args, const pos_idx_t pos) {}

void MultiEvalProfiler::pre_function_call_hook(eval_state_t& state, const value_t& v,
                                               std::span<value_t*> args, const pos_idx_t pos) {
  for (auto& profiler : profilers) {
    if (profiler->getNeededHooks().test(Hook::preFunctionCall)) {
      profiler->pre_function_call_hook(state, v, args, pos);
    }
  }
}

void MultiEvalProfiler::post_function_call_hook(eval_state_t& state, const value_t& v,
                                                std::span<value_t*> args, const pos_idx_t pos) {
  for (auto& profiler : profilers) {
    if (profiler->getNeededHooks().test(Hook::postFunctionCall)) {
      profiler->post_function_call_hook(state, v, args, pos);
    }
  }
}

EvalProfiler::Hooks MultiEvalProfiler::getNeededHooksImpl() const {
  Hooks hooks;
  for (auto& p : profilers) {
    hooks |= p->getNeededHooks();
  }
  return hooks;
}

void MultiEvalProfiler::addProfiler(ref<EvalProfiler> profiler) {
  profilers.push_back(profiler);
  invalidateNeededHooks();
}

namespace {

struct pos_cache_t {
  const eval_state_t& state;
  straylight::nix::data::LRUCache<pos_idx_t, pos_t, std::hash<pos_idx_t>> cache;

  pos_cache_t(const eval_state_t& state) : state(state), cache(524288) /* ~40MiB */ {}

  pos_t lookup(pos_idx_t pos_idx) {
    auto pos_or_none = cache.get(pos_idx);
    if (pos_or_none) {
      return *pos_or_none;
    }

    auto pos = state.positions[pos_idx];
    cache.put(pos_idx, pos);
    return pos;
  }
};

struct lambda_frame_info_t {
  ExprLambda* expr;
  /** Position where the lambda has been called. */
  pos_idx_t call_pos = no_pos;
  std::ostream& symbolize(const eval_state_t& state, std::ostream& os,
                          pos_cache_t& pos_cache) const;
  auto operator<=>(const lambda_frame_info_t& rhs) const = default;
};

/** Primop call. */
struct prim_op_frame_info_t {
  const PrimOp* expr;
  /** Position where the primop has been called. */
  pos_idx_t call_pos = no_pos;
  std::ostream& symbolize(const eval_state_t& state, std::ostream& os,
                          pos_cache_t& pos_cache) const;
  auto operator<=>(const prim_op_frame_info_t& rhs) const = default;
};

/** Used for functor calls (attrset with __functor attr). */
struct functor_frame_info_t {
  pos_idx_t pos;
  std::ostream& symbolize(const eval_state_t& state, std::ostream& os,
                          pos_cache_t& pos_cache) const;
  auto operator<=>(const functor_frame_info_t& rhs) const = default;
};

struct derivation_strict_frame_info_t {
  pos_idx_t call_pos = no_pos;
  std::string drv_name;
  std::ostream& symbolize(const eval_state_t& state, std::ostream& os,
                          pos_cache_t& pos_cache) const;
  auto operator<=>(const derivation_strict_frame_info_t& rhs) const = default;
};

/** Fallback frame info. */
struct generic_frame_info_t {
  pos_idx_t pos;
  std::ostream& symbolize(const eval_state_t& state, std::ostream& os,
                          pos_cache_t& pos_cache) const;
  auto operator<=>(const generic_frame_info_t& rhs) const = default;
};

using FrameInfo = std::variant<lambda_frame_info_t, prim_op_frame_info_t, functor_frame_info_t,
                               derivation_strict_frame_info_t, generic_frame_info_t>;
using FrameStack = std::vector<FrameInfo>;

/**
 * Stack sampling profiler.
 */
struct sample_stack_t : public EvalProfiler {
  /* How often stack profiles should be flushed to file. This avoids the need
     to persist stack samples across the whole evaluation at the cost
     of periodically flushing data to disk. */
  static constexpr std::chrono::microseconds profile_dump_interval =
      std::chrono::milliseconds(2000);

  Hooks getNeededHooksImpl() const override {
    return Hooks().set(preFunctionCall).set(postFunctionCall);
  }

  FrameInfo get_prim_op_frame_info(const PrimOp& prim_op, std::span<value_t*> args, pos_idx_t pos);

  sample_stack_t(eval_state_t& state, std::filesystem::path profile_file,
                 std::chrono::nanoseconds period)
      : state(state),
        sampleInterval(period),
        profileFd([&]() {
          auto_close_fd_t fd = to_descriptor(
              open(profile_file.string().c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0660));
          if (!fd) {
            throw sys_error_t("opening file %s", profile_file);
          }
          return fd;
        }()),
        pos_cache(state) {}

  [[gnu::noinline]] void pre_function_call_hook(eval_state_t& state, const value_t& v,
                                                std::span<value_t*> args,
                                                const pos_idx_t pos) override;
  [[gnu::noinline]] void post_function_call_hook(eval_state_t& state, const value_t& v,
                                                 std::span<value_t*> args,
                                                 const pos_idx_t pos) override;

  void maybe_save_profile(std::chrono::time_point<std::chrono::high_resolution_clock> now);
  void save_profile();
  FrameInfo get_frame_info_from_value_and_pos(const value_t& v, std::span<value_t*> args,
                                              pos_idx_t pos);

  sample_stack_t(sample_stack_t&&) = default;
  sample_stack_t& operator=(sample_stack_t&&) = delete;
  sample_stack_t(const sample_stack_t&) = delete;
  sample_stack_t& operator=(const sample_stack_t&) = delete;
  ~sample_stack_t();

  /** Hold on to an instance of eval_state_t for symbolizing positions. */
  eval_state_t& state;
  std::chrono::nanoseconds sampleInterval;
  auto_close_fd_t profileFd;
  FrameStack stack;
  std::map<FrameStack, uint32_t> callCount;
  std::chrono::time_point<std::chrono::high_resolution_clock> lastStackSample =
      std::chrono::high_resolution_clock::now();
  std::chrono::time_point<std::chrono::high_resolution_clock> lastDump =
      std::chrono::high_resolution_clock::now();
  pos_cache_t pos_cache;
};

FrameInfo sample_stack_t::get_prim_op_frame_info(const PrimOp& prim_op, std::span<value_t*> args,
                                                 pos_idx_t pos) {
  auto derivation_info = [&]() -> std::optional<FrameInfo> {
    /* Here we rely a bit on the implementation details of libexpr/primops/derivation.nix
       and derivationStrict primop. This is not ideal, but is necessary for
       the usefulness of the profiler. This might actually affect the evaluation,
       but the cost shouldn't be that high as to make the traces entirely inaccurate. */
    if (prim_op.name == "derivationStrict") {
      try {
        /* Error context strings don't actually matter, since we ignore all eval errors. */
        state.forceAttrs(*args[0], pos, "");
        auto attrs = args[0]->attrs();
        auto name_attr = state.get_attr(state.s.name, attrs, "");
        auto drv_name = std::string(state.forceStringNoCtx(*name_attr->value, pos, ""));
        return derivation_strict_frame_info_t{.call_pos = pos, .drv_name = std::move(drv_name)};
      } catch (...) {
        /* Ignore all errors, since those will be diagnosed by the evaluator itself. */
      }
    }

    return std::nullopt;
  }();

  return derivation_info.value_or(prim_op_frame_info_t{.expr = &prim_op, .call_pos = pos});
}

FrameInfo sample_stack_t::get_frame_info_from_value_and_pos(const value_t& v,
                                                            std::span<value_t*> args,
                                                            pos_idx_t pos) {
  /* NOTE: No actual references to garbage collected values are not held in
     the profiler. */
  if (v.isLambda()) {
    return lambda_frame_info_t{.expr = v.lambda().fun, .call_pos = pos};
  } else if (v.isPrimOp()) {
    return get_prim_op_frame_info(*v.prim_op(), args, pos);
  } else if (v.isPrimOpApp()) {
    /* Resolve prim_op eagerly. Must not hold on to a reference to a value_t. */
    return prim_op_frame_info_t{.expr = v.primOpAppPrimOp(), .call_pos = pos};
  } else if (state.isFunctor(v)) {
    const auto functor = v.attrs()->get(state.s.functor);
    if (auto pos_ = pos_cache.lookup(pos); std::holds_alternative<std::monostate>(pos_.origin)) {
      /* HACK: In case callsite position is unresolved. */
      return functor_frame_info_t{.pos = functor->pos};
    }
    return functor_frame_info_t{.pos = pos};
  } else {
    /* NOTE: Add a stack frame even for invalid cases (e.g. when calling a non-function). This is
     * what trace-function-calls does. */
    return generic_frame_info_t{.pos = pos};
  }
}

[[gnu::noinline]] void sample_stack_t::pre_function_call_hook(eval_state_t& state, const value_t& v,
                                                              std::span<value_t*> args,
                                                              const pos_idx_t pos) {
  stack.push_back(get_frame_info_from_value_and_pos(v, args, pos));

  auto now = std::chrono::high_resolution_clock::now();

  if (now - lastStackSample > sampleInterval) {
    callCount[stack] += 1;
    lastStackSample = now;
  }

  /* Do this in pre_function_call_hook because we might throw an exception, but
     callFunction uses finally_t, which doesn't play well with exceptions. */
  maybe_save_profile(now);
}

[[gnu::noinline]] void sample_stack_t::post_function_call_hook(eval_state_t& state,
                                                               const value_t& v,
                                                               std::span<value_t*> args,
                                                               const pos_idx_t pos) {
  if (!stack.empty()) {
    stack.pop_back();
  }
}

std::ostream& lambda_frame_info_t::symbolize(const eval_state_t& state, std::ostream& os,
                                             pos_cache_t& pos_cache) const {
  if (auto pos = pos_cache.lookup(call_pos); std::holds_alternative<std::monostate>(pos.origin)) {
    /* HACK: To avoid dubious «none»:0 in the generated profile if the origin can't be resolved
       resort to printing the lambda location instead of the callsite position. */
    os << pos_cache.lookup(expr->getPos());
  } else {
    os << pos;
  }
  if (expr->name) {
    os << ":" << state.symbols[expr->name];
  }
  return os;
}

std::ostream& generic_frame_info_t::symbolize(const eval_state_t& state, std::ostream& os,
                                              pos_cache_t& pos_cache) const {
  os << pos_cache.lookup(pos);
  return os;
}

std::ostream& functor_frame_info_t::symbolize(const eval_state_t& state, std::ostream& os,
                                              pos_cache_t& pos_cache) const {
  os << pos_cache.lookup(pos) << ":functor";
  return os;
}

std::ostream& prim_op_frame_info_t::symbolize(const eval_state_t& state, std::ostream& os,
                                              pos_cache_t& pos_cache) const {
  /* Sometimes callsite position can have an unresolved origin, which
     leads to confusing «none»:0 locations in the profile. */
  auto pos = pos_cache.lookup(call_pos);
  if (!std::holds_alternative<std::monostate>(pos.origin)) {
    os << pos_cache.lookup(call_pos) << ":";
  }
  os << *expr;
  return os;
}

std::ostream& derivation_strict_frame_info_t::symbolize(const eval_state_t& state, std::ostream& os,
                                                        pos_cache_t& pos_cache) const {
  /* Sometimes callsite position can have an unresolved origin, which
     leads to confusing «none»:0 locations in the profile. */
  auto pos = pos_cache.lookup(call_pos);
  if (!std::holds_alternative<std::monostate>(pos.origin)) {
    os << pos_cache.lookup(call_pos) << ":";
  }
  os << "primop derivationStrict:" << drv_name;
  return os;
}

void sample_stack_t::maybe_save_profile(
    std::chrono::time_point<std::chrono::high_resolution_clock> now) {
  if (now - lastDump >= profile_dump_interval) {
    save_profile();
  } else {
    return;
  }

  /* Save the last dump timepoint. Do this after actually saving data to file
     to not account for the time doing the flushing to disk. */
  lastDump = std::chrono::high_resolution_clock::now();

  /* Free up memory used for stack sampling. This might be very significant for
     long-running evaluations, so we shouldn't hog too much memory. */
  callCount.clear();
}

void sample_stack_t::save_profile() {
  auto os = std::ostringstream{};
  for (auto& [stack, count] : callCount) {
    auto first = true;
    for (auto& pos : stack) {
      if (first) {
        first = false;
      } else {
        os << ";";
      }

      std::visit([&](auto&& info) { info.symbolize(state, os, pos_cache); }, pos);
    }
    os << " " << count;
    write_line(profileFd.get(), os.str());
    /* Clear ostringstream. */
    os.str("");
    os.clear();
  }
}

sample_stack_t::~sample_stack_t() {
  /* Guard against cases when we are already unwinding the stack. */
  try {
    save_profile();
  } catch (...) {
    ignore_exception_in_destructor();
  }
}

} // namespace

ref<EvalProfiler> make_sample_stack_profiler(eval_state_t& state,
                                             std::filesystem::path profile_file,
                                             uint64_t frequency) {
  /* 0 is a special value for sampling stack after each call. */
  std::chrono::nanoseconds period =
      frequency == 0 ? std::chrono::nanoseconds{0}
                     : std::chrono::nanoseconds{std::nano::den / frequency / std::nano::num};
  return make_ref<sample_stack_t>(state, profile_file, period);
}

} // namespace nix
