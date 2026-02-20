#pragma once
///@file

#include <nlohmann/json_fwd.hpp>

#include "nix/util/error.h"
#include "nix/util/json-non-null.h"
#include "nix/util/types.h"

namespace nix {

/**
 * The list of available experimental features.
 *
 * If you update this, don’t forget to also change the map defining
 * their string representation and documentation in the corresponding
 * `.cc` file as well.
 */
enum struct experimental_feature_t {
  ca_derivations,
  impure_derivations,
  fetch_tree,
  git_hashing,
  recursive_nix,
  no_url_literals,
  fetch_closure,
  auto_allocate_uids,
  cgroups,
  daemon_trust_override,
  dynamic_derivations,
  parse_toml_timestamps,
  read_only_local_store,
  local_overlay_store,
  configurable_impure_env,
  mounted_ssh_store_t,
  verified_fetches,
  pipe_operators,
  external_builders,
  blak_e3_hashes,
  build_time_fetch_tree,
  parallel_eval,
};

extern std::set<std::string> stabilized_features;

/**
 * Just because writing `experimental_feature_t::ca_derivations` is way too long
 */
using xp_t = experimental_feature_t;

/**
 * Parse an experimental feature (enum value) from its name. Experimental
 * feature flag names are hyphenated and do not contain spaces.
 */
[[nodiscard]] auto parse_experimental_feature(const std::string_view& name)
    -> const std::optional<experimental_feature_t>;

/**
 * Show the name of an experimental feature. This is the opposite of
 * parse_experimental_feature().
 */
[[nodiscard]] auto show_experimental_feature(const experimental_feature_t) -> std::string_view;

/**
 * Compute the documentation of all experimental features.
 *
 * See `doc/manual` for how this information is used.
 */
[[nodiscard]] auto document_experimental_features() -> nlohmann::json;

/**
 * Shorthand for `str << show_experimental_feature(feature)`.
 */
auto operator<<(std::ostream& str, const experimental_feature_t& feature) -> std::ostream&;

/**
 * Parse a set of strings to the corresponding set of experimental
 * features, ignoring (but warning for) any unknown feature.
 */
[[nodiscard]] auto parse_features(const string_set_t&) -> std::set<experimental_feature_t>;

/**
 * An experimental feature was required for some (experimental)
 * operation, but was not enabled.
 */
class missing_experimental_feature_t : public Error {
public:
  /**
   * The experimental feature that was required but not enabled.
   */
  experimental_feature_t missing_feature;

  std::string reason;

  missing_experimental_feature_t(experimental_feature_t missing_feature, std::string reason = "");
};

/**
 * `experimental_feature_t` is always rendered as a string.
 */
template <>
struct json_avoids_null<experimental_feature_t> : std::true_type {};

/**
 * Semi-magic conversion to and from json.
 * See the nlohmann/json readme for more details.
 */
auto to_json(nlohmann::json&, const experimental_feature_t&) -> void;
auto from_json(const nlohmann::json&, experimental_feature_t&) -> void;

} // namespace nix
