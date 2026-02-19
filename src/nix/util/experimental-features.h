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
  CaDerivations,
  ImpureDerivations,
  FetchTree,
  GitHashing,
  RecursiveNix,
  NoUrlLiterals,
  FetchClosure,
  AutoAllocateUids,
  Cgroups,
  DaemonTrustOverride,
  DynamicDerivations,
  ParseTomlTimestamps,
  ReadOnlyLocalStore,
  LocalOverlayStore,
  ConfigurableImpureEnv,
  mounted_ssh_store_t,
  VerifiedFetches,
  PipeOperators,
  ExternalBuilders,
  BLAKE3Hashes,
  BuildTimeFetchTree,
  ParallelEval,
};

extern std::set<std::string> stabilizedFeatures;

/**
 * Just because writing `experimental_feature_t::CaDerivations` is way too long
 */
using xp_t = experimental_feature_t;

/**
 * Parse an experimental feature (enum value) from its name. Experimental
 * feature flag names are hyphenated and do not contain spaces.
 */
const std::optional<experimental_feature_t> parseExperimentalFeature(const std::string_view& name);

/**
 * Show the name of an experimental feature. This is the opposite of
 * parseExperimentalFeature().
 */
std::string_view showExperimentalFeature(const experimental_feature_t);

/**
 * Compute the documentation of all experimental features.
 *
 * See `doc/manual` for how this information is used.
 */
nlohmann::json documentExperimentalFeatures();

/**
 * Shorthand for `str << showExperimentalFeature(feature)`.
 */
std::ostream& operator<<(std::ostream& str, const experimental_feature_t& feature);

/**
 * Parse a set of strings to the corresponding set of experimental
 * features, ignoring (but warning for) any unknown feature.
 */
std::set<experimental_feature_t> parseFeatures(const string_set_t&);

/**
 * An experimental feature was required for some (experimental)
 * operation, but was not enabled.
 */
class missing_experimental_feature_t : public Error {
public:
  /**
   * The experimental feature that was required but not enabled.
   */
  experimental_feature_t missingFeature;

  std::string reason;

  missing_experimental_feature_t(experimental_feature_t missingFeature, std::string reason = "");
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
void to_json(nlohmann::json&, const experimental_feature_t&);
void from_json(const nlohmann::json&, experimental_feature_t&);

} // namespace nix
