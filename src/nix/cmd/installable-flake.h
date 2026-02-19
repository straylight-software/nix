#pragma once
///@file

#include "nix/cmd/common-eval-args.h"
#include "nix/cmd/installable-value.h"

namespace nix {

/**
 * Extra info about a \ref DerivedPath "derived path" that ultimately
 * come from a Flake.
 *
 * Invariant: every ExtraPathInfo gotten from an InstallableFlake should
 * be possible to downcast to an ExtraPathInfoFlake.
 */
struct ExtraPathInfoFlake : ExtraPathInfoValue {
  /**
   * Extra struct to get around C++ designated initializer limitations
   */
  struct Flake {
    FlakeRef original_ref;
    FlakeRef locked_ref;
  };

  Flake flake;

  ExtraPathInfoFlake(Value&& v, Flake&& f)
      : ExtraPathInfoValue(std::move(v)), flake(std::move(f)) {}
};

struct InstallableFlake : InstallableValue {
  FlakeRef flake_ref;
  strings_t attrPaths;
  strings_t prefixes;
  ExtendedOutputsSpec extendedOutputsSpec;
  const flake::LockFlags& lock_flags;
  mutable std::shared_ptr<flake::LockedFlake> _lockedFlake;

  InstallableFlake(SourceExprCommand* cmd, ref<EvalState> state, FlakeRef&& flake_ref,
                   std::string_view fragment, ExtendedOutputsSpec extendedOutputsSpec,
                   strings_t attrPaths, strings_t prefixes, const flake::LockFlags& lock_flags);

  std::string what() const override { return flake_ref.to_string() + "#" + *attrPaths.begin(); }

  std::vector<std::string> getActualAttrPaths();

  DerivedPathsWithInfo to_derived_paths() override;

  std::pair<Value*, pos_idx_t> toValue(EvalState& state) override;

  /**
   * Get a cursor to every attrpath in getActualAttrPaths() that
   * exists. However if none exists, throw an exception.
   */
  std::vector<ref<eval_cache::AttrCursor>> getCursors(EvalState& state) override;

  ref<flake::LockedFlake> getLockedFlake() const;

  FlakeRef nixpkgsFlakeRef() const;
};

/**
 * Default flake ref for referring to Nixpkgs. For flakes that don't
 * have their own Nixpkgs input, or other installables.
 *
 * It is a layer violation for Nix to know about Nixpkgs; currently just
 * `nix develop` does. Be wary of using this /
 * `InstallableFlake::nixpkgsFlakeRef` more places.
 */
static inline FlakeRef defaultNixpkgsFlakeRef() {
  return FlakeRef::fromAttrs(fetch_settings, {{"type", "indirect"}, {"id", "nixpkgs"}});
}

} // namespace nix
