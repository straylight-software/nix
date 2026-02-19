#include "nix/expr/eval-profiler.h"

#include "nix/expr/eval.h"
#include "nix/expr/nixexpr.h"
#include "nix/util/lru-cache.h"

namespace nix {

void EvalProfiler::preFunctionCallHook(EvalState& state, const Value& v, std::span<Value*> args,
                                       const pos_idx_t pos) {}

void EvalProfiler::postFunctionCallHook(EvalState& state, const Value& v, std::span<Value*> args,
                                        const pos_idx_t pos) {}

void MultiEvalProfiler::preFunctionCallHook(EvalState& state, const Value& v,
                                            std::span<Value*> args, const pos_idx_t pos) {
  for (auto& profiler : profilers) {
    if (profiler->getNeededHooks().test(Hook::preFunctionCall))
      profiler->preFunctionCallHook(state, v, args, pos);
  }
}

void MultiEvalProfiler::postFunctionCallHook(EvalState& state, const Value& v,
                                             std::span<Value*> args, const pos_idx_t pos) {
  for (auto& profiler : profilers) {
    if (profiler->getNeededHooks().test(Hook::postFunctionCall))
      profiler->postFunctionCallHook(state, v, args, pos);
  }
}

EvalProfiler::Hooks MultiEvalProfiler::getNeededHooksImpl() const {
  Hooks hooks;
  for (auto& p : profilers)
    hooks |= p->getNeededHooks();
  return hooks;
}

void MultiEvalProfiler::addProfiler(ref<EvalProfiler> profiler) {
  profilers.push_back(profiler);
  invalidateNeededHooks();
}

namespace {

class pos_cache_t : private lru_cache_t<pos_idx_t, Pos> {
  const EvalState& state;

public:
  pos_cache_t(const EvalState& state)
      : lru_cache_t(524288) /* ~40MiB */
        ,
        state(state) {}

  Pos lookup(pos_idx_t posIdx) {
    auto posOrNone = lru_cache_t::get(posIdx);
    if (posOrNone)
      return *posOrNone;

    auto pos = state.positions[posIdx];
    upsert(posIdx, pos);
    return pos;
  }
};

struct lambda_frame_info_t {
  ExprLambda* expr;
  /** Position where the lambda has been called. */
  pos_idx_t callPos = noPos;
  std::ostream& symbolize(const EvalState& state, std::ostream& os, pos_cache_t& posCache) const;
  auto operator<=>(const lambda_frame_info_t& rhs) const = default;
};

/** Primop call. */
struct prim_op_frame_info_t {
  const PrimOp* expr;
  /** Position where the primop has been called. */
  pos_idx_t callPos = noPos;
  std::ostream& symbolize(const EvalState& state, std::ostream& os, pos_cache_t& posCache) const;
  auto operator<=>(const prim_op_frame_info_t& rhs) const = default;
};

/** Used for functor calls (attrset with __functor attr). */
struct functor_frame_info_t {
  pos_idx_t pos;
  std::ostream& symbolize(const EvalState& state, std::ostream& os, pos_cache_t& posCache) const;
  auto operator<=>(const functor_frame_info_t& rhs) const = default;
};

struct derivation_strict_frame_info_t {
  pos_idx_t callPos = noPos;
  std::string drvName;
  std::ostream& symbolize(const EvalState& state, std::ostream& os, pos_cache_t& posCache) const;
  auto operator<=>(const derivation_strict_frame_info_t& rhs) const = default;
};

/** Fallback frame info. */
struct generic_frame_info_t {
  pos_idx_t pos;
  std::ostream& symbolize(const EvalState& state, std::ostream& os, pos_cache_t& posCache) const;
  auto operator<=>(const generic_frame_info_t& rhs) const = default;
};

using FrameInfo = std::variant<lambda_frame_info_t, prim_op_frame_info_t, functor_frame_info_t,
                               derivation_strict_frame_info_t, generic_frame_info_t>;
using FrameStack = std::vector<FrameInfo>;

/**
 * Stack sampling profiler.
 */
class sample_stack_t : public EvalProfiler {
  /* How often stack profiles should be flushed to file. This avoids the need
     to persist stack samples across the whole evaluation at the cost
     of periodically flushing data to disk. */
  static constexpr std::chrono::microseconds profileDumpInterval = std::chrono::milliseconds(2000);

  Hooks getNeededHooksImpl() const override {
    return Hooks().set(preFunctionCall).set(postFunctionCall);
  }

  FrameInfo getPrimOpFrameInfo(const PrimOp& primOp, std::span<Value*> args, pos_idx_t pos);

public:
  sample_stack_t(EvalState& state, std::filesystem::path profileFile, std::chrono::nanoseconds period)
      : state(state),
        sampleInterval(period),
        profileFd([&]() {
          auto_close_fd_t fd =
              toDescriptor(open(profileFile.string().c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0660));
          if (!fd)
            throw sys_error_t("opening file %s", profileFile);
          return fd;
        }()),
        posCache(state) {}

  [[gnu::noinline]] void preFunctionCallHook(EvalState& state, const Value& v,
                                             std::span<Value*> args, const pos_idx_t pos) override;
  [[gnu::noinline]] void postFunctionCallHook(EvalState& state, const Value& v,
                                              std::span<Value*> args, const pos_idx_t pos) override;

  void maybeSaveProfile(std::chrono::time_point<std::chrono::high_resolution_clock> now);
  void saveProfile();
  FrameInfo getFrameInfoFromValueAndPos(const Value& v, std::span<Value*> args, pos_idx_t pos);

  sample_stack_t(sample_stack_t&&) = default;
  sample_stack_t& operator=(sample_stack_t&&) = delete;
  sample_stack_t(const sample_stack_t&) = delete;
  sample_stack_t& operator=(const sample_stack_t&) = delete;
  ~sample_stack_t();

private:
  /** Hold on to an instance of EvalState for symbolizing positions. */
  EvalState& state;
  std::chrono::nanoseconds sampleInterval;
  auto_close_fd_t profileFd;
  FrameStack stack;
  std::map<FrameStack, uint32_t> callCount;
  std::chrono::time_point<std::chrono::high_resolution_clock> lastStackSample =
      std::chrono::high_resolution_clock::now();
  std::chrono::time_point<std::chrono::high_resolution_clock> lastDump =
      std::chrono::high_resolution_clock::now();
  pos_cache_t posCache;
};

FrameInfo sample_stack_t::getPrimOpFrameInfo(const PrimOp& primOp, std::span<Value*> args,
                                          pos_idx_t pos) {
  auto derivationInfo = [&]() -> std::optional<FrameInfo> {
    /* Here we rely a bit on the implementation details of libexpr/primops/derivation.nix
       and derivationStrict primop. This is not ideal, but is necessary for
       the usefulness of the profiler. This might actually affect the evaluation,
       but the cost shouldn't be that high as to make the traces entirely inaccurate. */
    if (primOp.name == "derivationStrict") {
      try {
        /* Error context strings don't actually matter, since we ignore all eval errors. */
        state.forceAttrs(*args[0], pos, "");
        auto attrs = args[0]->attrs();
        auto nameAttr = state.getAttr(state.s.name, attrs, "");
        auto drvName = std::string(state.forceStringNoCtx(*nameAttr->value, pos, ""));
        return derivation_strict_frame_info_t{.callPos = pos, .drvName = std::move(drvName)};
      } catch (...) {
        /* Ignore all errors, since those will be diagnosed by the evaluator itself. */
      }
    }

    return std::nullopt;
  }();

  return derivationInfo.value_or(prim_op_frame_info_t{.expr = &primOp, .callPos = pos});
}

FrameInfo sample_stack_t::getFrameInfoFromValueAndPos(const Value& v, std::span<Value*> args,
                                                   pos_idx_t pos) {
  /* NOTE: No actual references to garbage collected values are not held in
     the profiler. */
  if (v.isLambda())
    return lambda_frame_info_t{.expr = v.lambda().fun, .callPos = pos};
  else if (v.isPrimOp()) {
    return getPrimOpFrameInfo(*v.primOp(), args, pos);
  } else if (v.isPrimOpApp())
    /* Resolve primOp eagerly. Must not hold on to a reference to a Value. */
    return prim_op_frame_info_t{.expr = v.primOpAppPrimOp(), .callPos = pos};
  else if (state.isFunctor(v)) {
    const auto functor = v.attrs()->get(state.s.functor);
    if (auto pos_ = posCache.lookup(pos); std::holds_alternative<std::monostate>(pos_.origin))
      /* HACK: In case callsite position is unresolved. */
      return functor_frame_info_t{.pos = functor->pos};
    return functor_frame_info_t{.pos = pos};
  } else
    /* NOTE: Add a stack frame even for invalid cases (e.g. when calling a non-function). This is
     * what trace-function-calls does. */
    return generic_frame_info_t{.pos = pos};
}

[[gnu::noinline]] void sample_stack_t::preFunctionCallHook(EvalState& state, const Value& v,
                                                        std::span<Value*> args, const pos_idx_t pos) {
  stack.push_back(getFrameInfoFromValueAndPos(v, args, pos));

  auto now = std::chrono::high_resolution_clock::now();

  if (now - lastStackSample > sampleInterval) {
    callCount[stack] += 1;
    lastStackSample = now;
  }

  /* Do this in preFunctionCallHook because we might throw an exception, but
     callFunction uses finally_t, which doesn't play well with exceptions. */
  maybeSaveProfile(now);
}

[[gnu::noinline]] void sample_stack_t::postFunctionCallHook(EvalState& state, const Value& v,
                                                         std::span<Value*> args, const pos_idx_t pos) {
  if (!stack.empty())
    stack.pop_back();
}

std::ostream& lambda_frame_info_t::symbolize(const EvalState& state, std::ostream& os,
                                         pos_cache_t& posCache) const {
  if (auto pos = posCache.lookup(callPos); std::holds_alternative<std::monostate>(pos.origin))
    /* HACK: To avoid dubious «none»:0 in the generated profile if the origin can't be resolved
       resort to printing the lambda location instead of the callsite position. */
    os << posCache.lookup(expr->getPos());
  else
    os << pos;
  if (expr->name)
    os << ":" << state.symbols[expr->name];
  return os;
}

std::ostream& generic_frame_info_t::symbolize(const EvalState& state, std::ostream& os,
                                          pos_cache_t& posCache) const {
  os << posCache.lookup(pos);
  return os;
}

std::ostream& functor_frame_info_t::symbolize(const EvalState& state, std::ostream& os,
                                          pos_cache_t& posCache) const {
  os << posCache.lookup(pos) << ":functor";
  return os;
}

std::ostream& prim_op_frame_info_t::symbolize(const EvalState& state, std::ostream& os,
                                         pos_cache_t& posCache) const {
  /* Sometimes callsite position can have an unresolved origin, which
     leads to confusing «none»:0 locations in the profile. */
  auto pos = posCache.lookup(callPos);
  if (!std::holds_alternative<std::monostate>(pos.origin))
    os << posCache.lookup(callPos) << ":";
  os << *expr;
  return os;
}

std::ostream& derivation_strict_frame_info_t::symbolize(const EvalState& state, std::ostream& os,
                                                   pos_cache_t& posCache) const {
  /* Sometimes callsite position can have an unresolved origin, which
     leads to confusing «none»:0 locations in the profile. */
  auto pos = posCache.lookup(callPos);
  if (!std::holds_alternative<std::monostate>(pos.origin))
    os << posCache.lookup(callPos) << ":";
  os << "primop derivationStrict:" << drvName;
  return os;
}

void sample_stack_t::maybeSaveProfile(
    std::chrono::time_point<std::chrono::high_resolution_clock> now) {
  if (now - lastDump >= profileDumpInterval)
    saveProfile();
  else
    return;

  /* Save the last dump timepoint. Do this after actually saving data to file
     to not account for the time doing the flushing to disk. */
  lastDump = std::chrono::high_resolution_clock::now();

  /* Free up memory used for stack sampling. This might be very significant for
     long-running evaluations, so we shouldn't hog too much memory. */
  callCount.clear();
}

void sample_stack_t::saveProfile() {
  auto os = std::ostringstream{};
  for (auto& [stack, count] : callCount) {
    auto first = true;
    for (auto& pos : stack) {
      if (first)
        first = false;
      else
        os << ";";

      std::visit([&](auto&& info) { info.symbolize(state, os, posCache); }, pos);
    }
    os << " " << count;
    writeLine(profileFd.get(), os.str());
    /* Clear ostringstream. */
    os.str("");
    os.clear();
  }
}

sample_stack_t::~sample_stack_t() {
  /* Guard against cases when we are already unwinding the stack. */
  try {
    saveProfile();
  } catch (...) {
    ignoreExceptionInDestructor();
  }
}

} // namespace

ref<EvalProfiler> makeSampleStackProfiler(EvalState& state, std::filesystem::path profileFile,
                                          uint64_t frequency) {
  /* 0 is a special value for sampling stack after each call. */
  std::chrono::nanoseconds period =
      frequency == 0 ? std::chrono::nanoseconds{0}
                     : std::chrono::nanoseconds{std::nano::den / frequency / std::nano::num};
  return make_ref<sample_stack_t>(state, profileFile, period);
}

} // namespace nix
