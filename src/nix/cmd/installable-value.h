#pragma once
///@file

#include "nix/cmd/installables.h"
#include "nix/flake/flake.h"

namespace nix {

struct PackageInfo;
struct SourceExprCommand;

namespace eval_cache {
class EvalCache;
class AttrCursor;
} // namespace eval_cache

struct App {
  std::vector<derived_path_t> context;
  std::filesystem::path program;
  // FIXME: add args, sandbox settings, metadata, ...
};

struct UnresolvedApp {
  App unresolved;
  std::vector<BuiltPathWithResult> build(ref<store_t> eval_store, ref<store_t> store);
  App resolve(ref<store_t> eval_store, ref<store_t> store);
};

/**
 * Extra info about a \ref derived_path_t "derived path" that ultimately
 * come from a Nix language value.
 *
 * Invariant: every ExtraPathInfo gotten from an InstallableValue should
 * be possible to downcast to an ExtraPathInfoValue.
 */
struct ExtraPathInfoValue : ExtraPathInfo {
  /**
   * Extra struct to get around C++ designated initializer limitations
   */
  struct value_t {
    /**
     * An optional priority for use with "build envs". See Package
     */
    std::optional<NixInt::Inner> priority;

    /**
     * The attribute path associated with this value. The idea is
     * that an installable referring to a value typically refers to
     * a larger value, from which we project a smaller value out
     * with this.
     */
    std::string attr_path;

    /**
     * \todo merge with derived_path_t's 'outputs' field?
     */
    ExtendedOutputsSpec extendedOutputsSpec;
  };

  value_t value;

  ExtraPathInfoValue(value_t&& v) : value(std::move(v)) {}

  virtual ~ExtraPathInfoValue() = default;
};

/**
 * An Installable which corresponds a Nix language value, in addition to
 * a collection of \ref derived_path_t "derived paths".
 */
struct InstallableValue : Installable {
  ref<eval_state_t> state;

  InstallableValue(ref<eval_state_t> state) : state(state) {}

  virtual ~InstallableValue() {}

  virtual std::pair<value_t*, pos_idx_t> toValue(eval_state_t& state) = 0;

  /**
   * Get a cursor to each value this Installable could refer to.
   * However if none exists, throw exception instead of returning
   * empty vector.
   */
  virtual std::vector<ref<eval_cache::AttrCursor>> getCursors(eval_state_t& state);

  /**
   * Get the first and most preferred cursor this Installable could
   * refer to, or throw an exception if none exists.
   */
  virtual ref<eval_cache::AttrCursor> getCursor(eval_state_t& state);

  UnresolvedApp toApp(eval_state_t& state);

  static InstallableValue& require(Installable& installable);
  static ref<InstallableValue> require(ref<Installable> installable);

protected:
  /**
   * Handles either a plain path, or a string with a single string
   * context elem in the right format. The latter case is handled by
   * `eval_state_t::coerceToDerivedPath()`; see it for details.
   *
   * @param v value_t that is hopefully a string or path per the above.
   *
   * @param pos Position of value to aid with diagnostics.
   *
   * @param error_ctx Arbitrary message for use in potential error message when something is wrong
   * with `v`.
   *
   * @result A derived path (with empty info, for now) if the value
   * matched the above criteria.
   */
  std::optional<DerivedPathWithInfo> trySinglePathToDerivedPaths(value_t& v, const pos_idx_t pos,
                                                                 std::string_view error_ctx);
};

} // namespace nix
