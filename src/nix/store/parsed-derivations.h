#pragma once
///@file

#include <nlohmann/json.hpp>

#include "nix/store/path.h"
#include "nix/util/types.h"

namespace nix {

class store_t;
template <typename input_t>
struct derivation_options_t;
struct derivation_output_t;

using DerivationOutputs = std::map<std::string, derivation_output_t>;

struct StructuredAttrs {
  static constexpr std::string_view envVarName{"__json"};

  nlohmann::json::object_t structured_attrs;

  bool operator==(const StructuredAttrs&) const = default;

  /**
   * Unconditionally parse from a JSON string. Used by `tryExtract`.
   */
  static StructuredAttrs parse(std::string_view encoded);

  /**
   * Like `tryParse`, but removes the env var which encoded the structured
   * attrs from the map if one is found.
   */
  static std::optional<StructuredAttrs> tryExtract(string_pairs_t& env);

  /**
   * Opposite of `tryParse`, at least if one makes a map from this
   * single key-value PR.
   */
  std::pair<std::string_view, std::string> unparse() const;

  /**
   * Ensures that the structured attrs "env var" is not in used, so we
   * are free to use it instead.
   */
  static void checkKeyNotInUse(const string_pairs_t& env);

  nlohmann::json::object_t
  prepareStructuredAttrs(store_t& store, const derivation_options_t<store_path_t>& drv_options,
                         const store_path_set_t& inputPaths,
                         const DerivationOutputs& outputs) const;

  /**
   * As a convenience to bash scripts, write a shell file that
   * maps all attributes that are representable in bash -
   * namely, strings, integers, nulls, Booleans, and arrays and
   * objects consisting entirely of those values. (So nested
   * arrays or objects are not supported.)
   *
   * @param prepared This should be the result of
   * `prepareStructuredAttrs`, *not* the original `structured_attrs`
   * field.
   */
  static std::string writeShell(const nlohmann::json::object_t& prepared);
};

} // namespace nix
