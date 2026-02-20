#pragma once
///@file

#include <cstdint>
#include <optional>
#include <variant>

#include <nlohmann/json.hpp>

#include "nix/store/downstream-placeholder.h"
#include "nix/store/store-dir-config.h"
#include "nix/util/json-impls.h"
#include "nix/util/types.h"

namespace nix {

class store_t;
struct store_dir_config_t;
struct basic_derivation_t;
struct StructuredAttrs;

template <typename V>
struct DerivedPathMap;

/**
 * This represents all the special options on a `derivation_t`.
 *
 * Currently, these options are parsed from the environment variables
 * with the aid of `StructuredAttrs`.
 *
 * The first goal of this data type is to make sure that no other code
 * uses `StructuredAttrs` to ad-hoc parse some additional options. That
 * ensures this data type is up to date and fully correct.
 *
 * The second goal of this data type is to allow an alternative to
 * hackily parsing the options from the environment variables. The ATerm
 * format cannot change, but in alternatives to it (like the JSON
 * format), we have the option of instead storing the options
 * separately. That would be nice to separate concerns, and not make any
 * environment variable names magical.
 */
template <typename input_t>
struct derivation_options_t {
  struct OutputChecks {
    bool ignoreSelfRefs = false;
    std::optional<uint64_t> max_size, maxClosureSize;

    using DrvRef = nix::DrvRef<input_t>;

    /**
     * env: allowedReferences
     *
     * A value of `nullopt` indicates that the check is skipped.
     * This means that all references are allowed.
     */
    std::optional<std::set<DrvRef>> allowedReferences;

    /**
     * env: disallowedReferences
     *
     * No needed for `std::optional`, because skipping the check is
     * the same as disallowing the references.
     */
    std::set<DrvRef> disallowedReferences;

    /**
     * env: allowedRequisites
     *
     * See `allowedReferences`
     */
    std::optional<std::set<DrvRef>> allowedRequisites;

    /**
     * env: disallowedRequisites
     *
     * See `disallowedReferences`
     */
    std::set<DrvRef> disallowedRequisites;

    bool operator==(const OutputChecks&) const = default;
  };

  /**
   * Either one set of checks for all outputs, or separate checks
   * per-output.
   */
  std::variant<OutputChecks, std::map<std::string, OutputChecks>> output_checks = OutputChecks{};

  /**
   * Whether to avoid scanning for references for a given output.
   */
  std::map<std::string, bool> unsafeDiscardReferences;

  /**
   * In non-structured mode, all bindings specified in the derivation
   * go directly via the environment, except those listed in the
   * passAsFile attribute. Those are instead passed as file names
   * pointing to temporary files containing the contents.
   *
   * Note that passAsFile is ignored in structure mode because it's
   * not needed (attributes are not passed through the environment, so
   * there is no size constraint).
   */
  string_set_t passAsFile;

  /**
   * The `exportReferencesGraph' feature allows the references graph
   * to be passed to a builder
   *
   * ### Legacy case
   *
   * Given a `name` `pathSet` key-value pair, the references graph of
   * `pathSet` will be stored in a text file `name' in the temporary
   * build directory.  The text files have the format used by
   * `nix-store
   * --register-validity'.  However, the `deriver` fields are left
   *  empty.
   *
   * ### "Structured attributes" case
   *
   * The same information will be put put in the final structured
   * attributes give to the builder. The set of paths in the original JSON
   * is replaced with a list of `path_info_t` in JSON format.
   */
  std::map<std::string, std::set<input_t>> exportReferencesGraph;

  /**
   * env: __sandboxProfile
   *
   * Just for Darwin
   */
  std::string additionalSandboxProfile = "";

  /**
   * env: __noChroot
   *
   * derivation_t would like to opt out of the sandbox.
   *
   * Builder is free to not respect this wish (because it is
   * insecure) and fail the build instead.
   */
  bool noChroot = false;

  /**
   * env: __impureHostDeps
   */
  string_set_t impureHostDeps = {};

  /**
   * env: impureEnvVars
   */
  string_set_t impureEnvVars = {};

  /**
   * env: __darwinAllowLocalNetworking
   *
   * Just for Darwin
   */
  bool allowLocalNetworking = false;

  /**
   * env: requiredSystemFeatures
   */
  string_set_t requiredSystemFeatures = {};

  /**
   * env: preferLocalBuild
   */
  bool preferLocalBuild = false;

  /**
   * env: allowSubstitutes
   */
  bool allowSubstitutes = true;

  bool operator==(const derivation_options_t&) const = default;

  /**
   * @param drv Must be the same derivation we parsed this from. In
   * the future we'll flip things around so a `basic_derivation_t` has
   * `derivation_options_t` instead.
   */
  string_set_t getRequiredSystemFeatures(const basic_derivation_t& drv) const;

  /**
   * @param drv See note on `getRequiredSystemFeatures`
   */
  bool canBuildLocally(store_t& localStore, const basic_derivation_t& drv) const;

  /**
   * @param drv See note on `getRequiredSystemFeatures`
   */
  bool willBuildLocally(store_t& localStore, const basic_derivation_t& drv) const;

  bool substitutesAllowed() const;

  /**
   * @param drv See note on `getRequiredSystemFeatures`
   */
  bool useUidRange(const basic_derivation_t& drv) const;
};

extern template struct derivation_options_t<store_path_t>;
extern template struct derivation_options_t<SingleDerivedPath>;

struct derivation_output_t;

/**
 * Parse this information from its legacy encoding as part of the
 * environment. This should not be used with nice greenfield formats
 * (e.g. JSON) but is necessary for supporting old formats (e.g.
 * ATerm).
 */
derivation_options_t<SingleDerivedPath> derivation_options_from_structured_attrs(
    const store_dir_config_t& store, const DerivedPathMap<string_set_t>& input_drvs,
    const string_map_t& env, const StructuredAttrs* parsed, bool should_warn = true,
    const experimental_feature_settings_t& mock_xp_settings = experimental_feature_settings);

derivation_options_t<store_path_t> derivation_options_from_structured_attrs(
    const store_dir_config_t& store, const string_map_t& env, const StructuredAttrs* parsed,
    bool should_warn = true,
    const experimental_feature_settings_t& mock_xp_settings = experimental_feature_settings);

/**
 * This is the counterpart of `derivation_t::try_resolve`. In particular,
 * it takes the same sort of callback, which is used to reolve
 * non-constant deriving paths.
 *
 * We need this function when resolving a derivation, and we will use
 * this as part of that if/when `derivation_t` includes
 * `derivation_options_t`
 */
std::optional<derivation_options_t<store_path_t>>
try_resolve(const derivation_options_t<SingleDerivedPath>& drv_options,
            std::function<std::optional<store_path_t>(ref<const SingleDerivedPath> drv_path,
                                                      const std::string& output_name)>
                queryResolutionChain);

}; // namespace nix

JSON_IMPL(nix::derivation_options_t<nix::store_path_t>);
JSON_IMPL(nix::derivation_options_t<nix::SingleDerivedPath>);
JSON_IMPL(nix::derivation_options_t<nix::store_path_t>::OutputChecks)
JSON_IMPL(nix::derivation_options_t<nix::SingleDerivedPath>::OutputChecks)
