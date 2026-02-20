#include "nix/cmd/installable-value.h"

#include "nix/expr/eval-cache.h"
#include "nix/fetchers/fetch-to-store.h"

namespace nix {

std::vector<ref<eval_cache::AttrCursor>> InstallableValue::getCursors(eval_state_t& state) {
  auto eval_cache = std::make_shared<nix::eval_cache::EvalCache>(
      std::nullopt, state, [&]() { return toValue(state).first; });
  return {eval_cache->get_root()};
}

ref<eval_cache::AttrCursor> InstallableValue::getCursor(eval_state_t& state) {
  /* Although getCursors should return at least one element, in case it doesn't,
     bound check to avoid an undefined behavior for vector[0] */
  return getCursors(state).at(0);
}

static UsageError non_value_installable(Installable& installable) {
  return UsageError("installable '%s' does not correspond to a Nix language value",
                    installable.what());
}

InstallableValue& InstallableValue::require(Installable& installable) {
  auto* castedInstallable = dynamic_cast<InstallableValue*>(&installable);
  if (!castedInstallable)
    throw non_value_installable(installable);
  return *castedInstallable;
}

ref<InstallableValue> InstallableValue::require(ref<Installable> installable) {
  auto castedInstallable = installable.dynamic_pointer_cast<InstallableValue>();
  if (!castedInstallable)
    throw non_value_installable(*installable);
  return ref{castedInstallable};
}

std::optional<DerivedPathWithInfo>
InstallableValue::trySinglePathToDerivedPaths(value_t& v, const pos_idx_t pos,
                                              std::string_view error_ctx) {
  if (v.type() == nPath) {
    auto store_path =
        fetch_to_store(state->fetch_settings, *state->store, v.path(), FetchMode::Copy);
    return {{
        .path =
            derived_path_t::opaque_t{
                .path = std::move(store_path),
            },
        .info = make_ref<ExtraPathInfo>(),
    }};
  }

  else if (v.type() == nString) {
    return {{
        .path = derived_path_t::fromSingle(
            state->devirtualize(state->coerceToSingleDerivedPath(pos, v, error_ctx))),
        .info = make_ref<ExtraPathInfo>(),
    }};
  }

  else
    return std::nullopt;
}

} // namespace nix
