#pragma once
///@file

#include <variant>

#include <boost/unordered/concurrent_flat_map_fwd.hpp>

#include "nix/store/content-address.h"
#include "nix/store/derived-path-map.h"
#include "nix/store/parsed-derivations.h"
#include "nix/store/path.h"
#include "nix/util/hash.h"
#include "nix/util/repair-flag.h"
#include "nix/util/sync.h"
#include "nix/util/types.h"
#include "nix/util/variant-wrapper.h"

namespace nix {

struct store_dir_config_t;
struct AsyncPathWriter;

/* Abstract syntax of derivations. */

/**
 * A single output of a basic_derivation_t (and derivation_t).
 */
struct derivation_output_t {
  /**
   * The traditional non-fixed-output derivation type.
   */
  struct InputAddressed {
    store_path_t path;

    bool operator==(const InputAddressed&) const = default;
    auto operator<=>(const InputAddressed&) const = default;
  };

  /**
   * Fixed-output derivations, whose output paths are content
   * addressed according to that fixed output.
   */
  struct CAFixed {
    /**
     * Method and hash used for expected hash computation.
     *
     * References are not allowed by fiat.
     */
    content_address_t ca;

    /**
     * Return the \ref store_path_t "store path" corresponding to this output
     *
     * @param drv_name The name of the derivation this is an output of, without the `.drv`.
     * @param output_name The name of this output.
     */
    store_path_t path(const store_dir_config_t& store, std::string_view drv_name,
                   OutputNameView output_name) const;

    bool operator==(const CAFixed&) const = default;
    auto operator<=>(const CAFixed&) const = default;
  };

  /**
   * Floating-output derivations, whose output paths are content
   * addressed, but not fixed, and so are dynamically calculated from
   * whatever the output ends up being.
   * */
  struct CAFloating {
    /**
     * How the file system objects will be serialized for hashing
     */
    content_address_method_t method;

    /**
     * How the serialization will be hashed
     */
    hash_algorithm_t hash_algo;

    bool operator==(const CAFloating&) const = default;
    auto operator<=>(const CAFloating&) const = default;
  };

  /**
   * input_t-addressed output which depends on a (CA) derivation whose hash
   * isn't known yet.
   */
  struct Deferred {
    bool operator==(const Deferred&) const = default;
    auto operator<=>(const Deferred&) const = default;
  };

  /**
   * Impure output which is moved to a content-addressed location (like
   * CAFloating) but isn't registered as a realization.
   */
  struct Impure {
    /**
     * How the file system objects will be serialized for hashing
     */
    content_address_method_t method;

    /**
     * How the serialization will be hashed
     */
    hash_algorithm_t hash_algo;

    bool operator==(const Impure&) const = default;
    auto operator<=>(const Impure&) const = default;
  };

  typedef std::variant<InputAddressed, CAFixed, CAFloating, Deferred, Impure> raw_t;

  raw_t raw;

  bool operator==(const derivation_output_t&) const = default;
  auto operator<=>(const derivation_output_t&) const = default;

  MAKE_WRAPPER_CONSTRUCTOR(derivation_output_t);

  /**
   * Force choosing a variant
   */
  derivation_output_t() = delete;

  /**
   * \note when you use this function you should make sure that you're
   * passing the right derivation name. When in doubt, you should use
   * the safer interface provided by
   * basic_derivation_t::outputsAndOptPaths
   */
  std::optional<store_path_t> path(const store_dir_config_t& store, std::string_view drv_name,
                                OutputNameView output_name) const;
};

using DerivationOutputs = std::map<std::string, derivation_output_t>;

/**
 * These are analogues to the previous DerivationOutputs data type,
 * but they also contains, for each output, the (optional) store
 * path in which it would be written. To calculate values of these
 * types, see the corresponding functions in basic_derivation_t.
 */
typedef std::map<std::string, std::pair<derivation_output_t, std::optional<store_path_t>>>
    DerivationOutputsAndOptPaths;

/**
 * For inputs that are sub-derivations, we specify exactly which
 * output IDs we are interested in.
 */
using DerivationInputs = std::map<store_path_t, string_set_t>;

struct DerivationType {
  /**
   * input_t-addressed derivation types
   */
  struct InputAddressed {
    /**
     * True iff the derivation type can't be determined statically,
     * for instance because it (transitively) depends on a content-addressed
     * derivation.
     */
    bool deferred;

    bool operator==(const InputAddressed&) const = default;
    auto operator<=>(const InputAddressed&) const = default;
  };

  /**
   * Content-addressing derivation types
   */
  struct ContentAddressed {
    /**
     * Whether the derivation should be built safely inside a sandbox.
     */
    bool sandboxed;
    /**
     * Whether the derivation's outputs' content-addresses are "fixed"
     * or "floating".
     *
     *  - Fixed: content-addresses are written down as part of the
     *    derivation itself. If the outputs don't end up matching the
     *    build fails.
     *
     *  - Floating: content-addresses are not written down, we do not
     *    know them until we perform the build.
     */
    bool fixed;

    bool operator==(const ContentAddressed&) const = default;
    auto operator<=>(const ContentAddressed&) const = default;
  };

  /**
   * Impure derivation type
   *
   * This is similar at build-time to the content addressed, not standboxed, not fixed
   * type, but has some restrictions on its usage.
   */
  struct Impure {
    bool operator==(const Impure&) const = default;
    auto operator<=>(const Impure&) const = default;
  };

  typedef std::variant<InputAddressed, ContentAddressed, Impure> raw_t;

  raw_t raw;

  bool operator==(const DerivationType&) const = default;
  auto operator<=>(const DerivationType&) const = default;

  MAKE_WRAPPER_CONSTRUCTOR(DerivationType);

  /**
   * Force choosing a variant
   */
  DerivationType() = delete;

  /**
   * Do the outputs of the derivation have paths calculated from their
   * content, or from the derivation itself?
   */
  bool isCA() const;

  /**
   * Is the content of the outputs fixed <em>a priori</em> via a hash?
   * Never true for non-CA derivations.
   */
  bool isFixed() const;

  /**
   * Whether the derivation is fully sandboxed. If false, the sandbox
   * is opened up, e.g. the derivation has access to the network. Note
   * that whether or not we actually sandbox the derivation is
   * controlled separately. always true for non-CA derivations.
   */
  bool isSandboxed() const;

  /**
   * Whether the derivation is expected to produce a different result
   * every time, and therefore it needs to be rebuilt every time. This is
   * only true for derivations that have the attribute '__impure =
   * true'.
   *
   * Non-impure derivations can still behave impurely, to the degree permitted
   * by the sandbox. Hence why this method isn't `isPure`: impure derivations
   * are not the negation of pure derivations. Purity can not be ascertained
   * except by rather heavy tools.
   */
  bool is_impure() const;

  /**
   * Does the derivation knows its own output paths?
   * Only true when there's no floating-ca derivation involved in the
   * closure, or if fixed output.
   */
  bool hasKnownOutputPaths() const;
};

struct basic_derivation_t {
  /**
   * keyed on symbolic IDs
   */
  DerivationOutputs outputs;
  /**
   * inputs that are sources
   */
  store_path_set_t input_srcs;
  std::string platform;
  Path builder;
  strings_t args;
  /**
   * Must not contain the key `__json`, at least in order to serialize to ATerm.
   */
  string_pairs_t env;
  std::optional<StructuredAttrs> structured_attrs;

  std::string name;

  basic_derivation_t() = default;
  basic_derivation_t(basic_derivation_t&&) = default;
  basic_derivation_t(const basic_derivation_t&) = default;
  basic_derivation_t& operator=(basic_derivation_t&&) = default;
  basic_derivation_t& operator=(const basic_derivation_t&) = default;
  virtual ~basic_derivation_t() {};

  bool isBuiltin() const;

  /**
   * Return true iff this is a fixed-output derivation.
   */
  DerivationType type() const;

  /**
   * Return the output names of a derivation.
   */
  string_set_t outputNames() const;

  /**
   * Calculates the maps that contains all the DerivationOutputs, but
   * augmented with knowledge of the store_t paths they would be written
   * into.
   */
  DerivationOutputsAndOptPaths outputsAndOptPaths(const store_dir_config_t& store) const;

  static std::string_view nameFromPath(const store_path_t& store_path);

  /**
   * Apply string rewrites to the `env`, `args` and `builder`
   * fields.
   */
  void applyRewrites(const string_map_t& rewrites);

  bool operator==(const basic_derivation_t&) const = default;
  // TODO libc++ 16 (used by darwin) missing `std::map::operator <=>`, can't do yet.
  // auto operator <=> (const basic_derivation_t &) const = default;
};

class store_t;

struct derivation_t : basic_derivation_t {
  /**
   * inputs that are sub-derivations
   */
  DerivedPathMap<std::set<OutputName, std::less<>>> input_drvs;

  /**
   * Print a derivation.
   */
  std::string unparse(const store_dir_config_t& store, bool mask_outputs,
                      DerivedPathMap<string_set_t>::ChildNode::Map* actualInputs = nullptr) const;

  /**
   * Return the underlying basic derivation but with these changes:
   *
   * 1. input_t drvs are emptied, but the outputs of them that were used
   *    are added directly to input sources.
   *
   * 2. input_t placeholders are replaced with realized input store
   *    paths.
   */
  std::optional<basic_derivation_t> try_resolve(store_t& store, store_t* eval_store = nullptr) const;

  /**
   * Like the above, but instead of querying the Nix database for
   * realisations, uses a given mapping from input derivation paths +
   * output names to actual output store paths.
   */
  std::optional<basic_derivation_t>
  try_resolve(store_t& store,
             std::function<std::optional<store_path_t>(ref<const SingleDerivedPath> drv_path,
                                                    const std::string& output_name)>
                 queryResolutionChain) const;

  /**
   * Check that the derivation is valid and does not present any
   * illegal states.
   *
   * This is mainly a matter of checking the outputs, where our C++
   * representation supports all sorts of combinations we do not yet
   * allow.
   *
   * This overload does not validate the derivation name or add path
   * context to errors. use this when you don't have a `store_path_t` or
   * when you want to handle error context yourself.
   *
   * @param store The store to use for validation
   */
  void checkInvariants(store_t& store) const;

  /**
   * This overload does everything the base `checkInvariants` does,
   * but also validates that the derivation name matches the path, and
   * improves any error messages that occur using the derivation path.
   *
   * @param store The store to use for validation
   * @param drv_path The path to this derivation
   */
  void checkInvariants(store_t& store, const store_path_t& drv_path) const;

  /**
   * Fill in output paths as needed.
   *
   * For input-addressed derivations (ready or deferred), it computes
   * the derivation hash modulo and based on the result:
   *
   * - If `regular`: converts `Deferred` outputs to `InputAddressed`,
   *   and ensures all `InputAddressed` outputs (whether preexisting
   *   or newly computed) have the right computed paths. Likewise
   *   defines (if absent or the empty string) or checks (if
   *   preexisting and non-empty) environment variables for each
   *   output with their path.
   *
   * - If `Deferred`: converts `InputAddressed` to `Deferred`.
   *
   * Also for fixed-output content-addressed derivations, likewise
   * updates output paths in env vars.
   *
   * @param store The store to use for path computation
   * @param drv_name The derivation name (without .drv extension)
   */
  void fillInOutputPaths(store_t& store);

  derivation_t() = default;

  derivation_t(const basic_derivation_t& bd) : basic_derivation_t(bd) {}

  derivation_t(basic_derivation_t&& bd) : basic_derivation_t(std::move(bd)) {}

  /**
   * Parse a derivation from JSON, and also perform various
   * conveniences such as:
   *
   * 1. Filling in output paths in as needed/required.
   *
   * 2. Checking invariants in general.
   *
   * In the future it might also do things like:
   *
   * - assist with the migration from older JSON formats.
   *
   * - (a somewhat example of the above) initialize
   *   `derivation_options_t` from their traditional encoding inside the
   *   `env` and `structured_attrs`.
   *
   * @param store The store to use for path computation and validation
   * @param json The JSON representation of the derivation
   * @return A validated derivation with output paths filled in
   * @throws Error if parsing fails, output paths can't be computed, or validation fails
   */
  static derivation_t parseJsonAndValidate(store_t& store, const nlohmann::json& json);

  bool operator==(const derivation_t&) const = default;
  // TODO libc++ 16 (used by darwin) missing `std::map::operator <=>`, can't do yet.
  // auto operator <=> (const derivation_t &) const = default;
};

class store_t;

/**
 * Write a derivation to the Nix store, and return its path.
 */
store_path_t write_derivation(store_t& store, const derivation_t& drv, RepairFlag repair = NoRepair,
                          bool read_only = false);

/**
 * Asynchronously write a derivation to the Nix store, and return its path.
 */
store_path_t write_derivation(store_t& store, AsyncPathWriter& async_path_writer, const derivation_t& drv,
                          RepairFlag repair = NoRepair, bool read_only = false);

/**
 * Read a derivation from a file.
 */
derivation_t
parse_derivation(const store_dir_config_t& store, std::string&& s, std::string_view name,
                const experimental_feature_settings_t& xp_settings = experimental_feature_settings);

/**
 * \todo Remove.
 *
 * use Path::is_derivation instead.
 */
bool is_derivation(std::string_view file_name);

/**
 * Calculate the name that will be used for the store path for this
 * output.
 *
 * This is usually <drv-name>-<output-name>, but is just <drv-name> when
 * the output name is "out".
 */
std::string output_path_name(std::string_view drv_name, OutputNameView output_name);

/**
 * The hashes modulo of a derivation.
 *
 * Each output is given a hash, although in practice only the content-addressed
 * derivations (fixed-output or not) will have a different hash for each
 * output.
 */
struct DrvHash {
  /**
   * Map from output names to hashes
   */
  std::map<std::string, Hash> hashes;

  enum struct Kind : bool {
    /**
     * Statically determined derivations.
     * This hash will be directly used to compute the output paths
     */
    regular,

    /**
     * Floating-output derivations (and their reverse dependencies).
     */
    Deferred,
  };

  /**
   * The kind of derivation this is, simplified for just "derivation hash
   * modulo" purposes.
   */
  Kind kind;
};

void operator|=(DrvHash::Kind& self, const DrvHash::Kind& other) noexcept;

/**
 * Returns hashes with the details of fixed-output subderivations
 * expunged.
 *
 * A fixed-output derivation is a derivation whose outputs have a
 * specified content hash and hash algorithm. (Currently they must have
 * exactly one output (`out`), which is specified using the `outputHash`
 * and `outputHashAlgo` attributes, but the algorithm doesn't assume
 * this.) We don't want changes to such derivations to propagate upwards
 * through the dependency graph, changing output paths everywhere.
 *
 * For instance, if we change the url in a call to the `fetchurl`
 * function, we do not want to rebuild everything depending on it---after
 * all, (the hash of) the file being downloaded is unchanged.  So the
 * *output paths* should not change. On the other hand, the *derivation
 * paths* should change to reflect the new dependency graph.
 *
 * For fixed-output derivations, this returns a map from the name of
 * each output to its hash, unique up to the output's contents.
 *
 * For regular derivations, it returns a single hash of the derivation
 * ATerm, after subderivations have been likewise expunged from that
 * derivation.
 */
DrvHash hash_derivation_modulo(store_t& store, const derivation_t& drv, bool mask_outputs);

/**
 * Return a map associating each output to a hash that uniquely identifies its
 * derivation (modulo the self-references).
 *
 * \todo What is the Hash in this map?
 */
std::map<std::string, Hash> static_output_hashes(store_t& store, const derivation_t& drv);

struct DrvHashFct {
  using is_avalanching = std::true_type;

  std::size_t operator()(const store_path_t& path) const noexcept {
    return std::hash<std::string_view>{}(path.to_string());
  }
};

/**
 * Memoisation of hash_derivation_modulo().
 */
using DrvHashes = boost::concurrent_flat_map<store_path_t, DrvHash, DrvHashFct>;

// FIXME: global, though at least thread-safe.
extern DrvHashes drv_hashes;

struct source_t;
struct sink_t;

source_t& read_derivation(source_t& in, const store_dir_config_t& store, basic_derivation_t& drv,
                       std::string_view name);
void write_derivation(sink_t& out, const store_dir_config_t& store, const basic_derivation_t& drv);

/**
 * This creates an opaque and almost certainly unique string
 * deterministically from the output name.
 *
 * It is used as a placeholder to allow derivations to refer to their
 * own outputs without needing to use the hash of a derivation in
 * itself, making the hash near-impossible to calculate.
 */
std::string hash_placeholder(const OutputNameView output_name);

/**
 * The expected JSON version for derivation serialization.
 * Used by `nix derivation show` and `nix derivation add`.
 */
constexpr unsigned expectedJsonVersionDerivation = 4;

} // namespace nix

JSON_IMPL_WITH_XP_FEATURES(nix::derivation_output_t)
JSON_IMPL_WITH_XP_FEATURES(nix::derivation_t)
