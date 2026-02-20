#pragma once
///@file

#include <variant>

#include <nlohmann/json_fwd.hpp>

#include "nix/store/derived-path.h"
#include "nix/store/path.h"
#include "nix/util/comparator.h"
#include "nix/util/hash.h"
#include "nix/util/signature/signer.h"

namespace nix {

class store_t;
struct OutputsSpec;

/**
 * A general `realisation_t` key.
 *
 * This is similar to a `derived_path_t::opaque_t`, but the derivation is
 * identified by its "hash modulo" instead of by its store path.
 */
struct DrvOutput {
  /**
   * The hash modulo of the derivation.
   *
   * Computed from the derivation itself for most types of
   * derivations, but computed from the (fixed) content address of the
   * output for fixed-output derivations.
   */
  Hash drvHash;

  /**
   * The name of the output.
   */
  OutputName output_name;

  std::string to_string() const;

  std::string strHash() const { return drvHash.to_string(hash_format_t::base16, true); }

  static DrvOutput parse(const std::string&);

  bool operator==(const DrvOutput&) const = default;
  auto operator<=>(const DrvOutput&) const = default;
};

struct UnkeyedRealisation {
  store_path_t out_path;

  string_set_t signatures;

  /**
   * The realisations that are required for the current one to be valid.
   *
   * When importing this realisation, the store will first check that all its
   * dependencies exist, and map to the correct output path
   */
  std::map<DrvOutput, store_path_t> dependentRealisations;

  std::string fingerprint(const DrvOutput& key) const;

  void sign(const DrvOutput& key, const signer_t&);

  bool checkSignature(const DrvOutput& key, const public_keys_t& public_keys,
                      const std::string& sig) const;

  size_t checkSignatures(const DrvOutput& key, const public_keys_t& public_keys) const;

  const store_path_t& get_path() const { return out_path; }

  // TODO sketchy that it avoids signatures
  GENERATE_CMP(UnkeyedRealisation, me->out_path);
};

struct realisation_t : UnkeyedRealisation {
  DrvOutput id;

  bool isCompatibleWith(const UnkeyedRealisation& other) const;

  static std::set<realisation_t> closure(store_t&, const std::set<realisation_t>&);

  static void closure(store_t&, const std::set<realisation_t>&, std::set<realisation_t>& res);

  bool operator==(const realisation_t&) const = default;
  auto operator<=>(const realisation_t&) const = default;
};

/**
 * Collection type for a single derivation's outputs' `realisation_t`s.
 *
 * Since these are the outputs of a single derivation, we know the
 * output names are unique so we can use them as the map key.
 */
using SingleDrvOutputs = std::map<OutputName, realisation_t>;

/**
 * Collection type for multiple derivations' outputs' `realisation_t`s.
 *
 * `DrvOutput` is used because in general the derivations are not all
 * the same, so we need to identify firstly which derivation, and
 * secondly which output of that derivation.
 */
using DrvOutputs = std::map<DrvOutput, realisation_t>;

struct OpaquePath {
  store_path_t path;

  const store_path_t& get_path() const& { return path; }

  bool operator==(const OpaquePath&) const = default;
  auto operator<=>(const OpaquePath&) const = default;
};

/**
 * A store path with all the history of how it went into the store
 */
struct RealisedPath {
  /**
   * A path is either the result of the realisation of a derivation or
   * an opaque blob that has been directly added to the store
   */
  using raw_t = std::variant<realisation_t, OpaquePath>;
  raw_t raw;

  using Set = std::set<RealisedPath>;

  RealisedPath(store_path_t path) : raw(OpaquePath{path}) {}

  RealisedPath(realisation_t r) : raw(r) {}

  /**
   * Get the raw store path associated to this
   */
  const store_path_t& path() const&;

  void closure(store_t& store, Set& ret) const;
  static void closure(store_t& store, const Set& startPaths, Set& ret);
  Set closure(store_t& store) const;

  bool operator==(const RealisedPath&) const = default;
  auto operator<=>(const RealisedPath&) const = default;
};

class MissingRealisation : public Error {
public:
  MissingRealisation(DrvOutput& output_id)
      : MissingRealisation(output_id.output_name, output_id.strHash()) {}

  MissingRealisation(std::string_view drv, OutputName output_name)
      : Error("cannot operate on output '%s' of the "
              "unbuilt derivation '%s'",
              output_name, drv) {}
};

} // namespace nix

JSON_IMPL(nix::DrvOutput)
JSON_IMPL(nix::UnkeyedRealisation)
JSON_IMPL(nix::realisation_t)
