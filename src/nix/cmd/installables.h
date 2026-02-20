#pragma once
///@file

#include <optional>

#include "nix/cmd/built-path.h"
#include "nix/store/build-result.h"
#include "nix/store/derived-path.h"
#include "nix/store/outputs-spec.h"
#include "nix/store/path.h"
#include "nix/store/store-api.h"

namespace nix {

struct PackageInfo;

enum class Realise {
  /**
   * Build the derivation.
   *
   * Postcondition: the derivation outputs exist.
   */
  Outputs,
  /**
   * Don't build the derivation.
   *
   * Postcondition: the store derivation exists.
   */
  derivation_t,
  /**
   * Evaluate in dry-run mode.
   *
   * Postcondition: nothing.
   *
   * \todo currently unused, but could be revived if we can evaluate
   * derivations in-memory.
   */
  Nothing
};

/**
 * How to handle derivations in commands that operate on store paths.
 */
enum class OperateOn {
  /**
   * Operate on the output path.
   */
  Output,
  /**
   * Operate on the .drv path.
   */
  derivation_t
};

/**
 * Extra info about a derived_path_t
 *
 * yes, this is empty, but that is intended. It will be sub-classed by
 * the subclasses of Installable to allow those to provide more info.
 * Certain commands will make use of this info.
 */
struct ExtraPathInfo {
  virtual ~ExtraPathInfo() = default;
};

/**
 * A derived_path_t with \ref ExtraPathInfo "any additional info" that
 * commands might need from the derivation.
 */
struct DerivedPathWithInfo {
  derived_path_t path;
  ref<ExtraPathInfo> info;
};

/**
 * Like DerivedPathWithInfo but extending BuiltPath with \ref
 * ExtraPathInfo "extra info" and also possibly the \ref build_result_t
 * "result of building".
 */
struct BuiltPathWithResult {
  BuiltPath path;
  ref<ExtraPathInfo> info;
  std::optional<build_result_t> result;
};

BuiltPaths to_built_paths(const std::vector<BuiltPathWithResult>& built_paths_with_result);

/**
 * Shorthand, for less typing and helping us keep the choice of
 * collection in sync.
 */
using DerivedPathsWithInfo = std::vector<DerivedPathWithInfo>;

struct Installable;

struct InstallableWithBuildResult {
  ref<Installable> installable;

  using Success = BuiltPathWithResult;

  using Failure = build_result_t; // must be a `build_result_t::Failure`

  std::variant<Success, Failure> result;

  /**
   * Throw an exception if this represents a failure, otherwise returns a `BuiltPathWithResult`.
   */
  const BuiltPathWithResult& getSuccess() const;
};

/**
 * Shorthand, for less typing and helping us keep the choice of
 * collection in sync.
 */
using Installables = std::vector<ref<Installable>>;

/**
 * Installables are the main positional arguments for the Nix
 * command_t-line.
 *
 * This base class is very flexible, and just assumes and the
 * Installable refers to a collection of \ref derived_path_t "derived paths" with
 * \ref ExtraPathInfo "extra info".
 */
struct Installable {
  virtual ~Installable() {}

  /**
   * What Installable is this?
   *
   * Prints back valid CLI syntax that would result in this same
   * installable. It doesn't need to be exactly what the user wrote,
   * just something that means the same thing.
   */
  virtual std::string what() const = 0;

  /**
   * Get the collection of \ref DerivedPathWithInfo "derived paths
   * with info" that this \ref Installable instalallable denotes.
   *
   * This is the main method of this class
   */
  virtual DerivedPathsWithInfo to_derived_paths() = 0;

  /**
   * A convenience wrapper of the above for when we expect an
   * installable to produce a single \ref derived_path_t "derived path"
   * only.
   *
   * If no or multiple \ref derived_path_t "derived paths" are produced,
   * and error is raised.
   */
  DerivedPathWithInfo toDerivedPath();

  /**
   * Return a value only if this installable is a store path or a
   * symlink to it.
   *
   * \todo should we move this to InstallableDerivedPath? It is only
   * supposed to work there anyways. Can always downcast.
   */
  virtual std::optional<store_path_t> getStorePath() { return {}; }

  static std::vector<BuiltPathWithResult> build(ref<store_t> eval_store, ref<store_t> store,
                                                Realise mode, const Installables& installables,
                                                BuildMode bMode = bmNormal);

  static std::vector<InstallableWithBuildResult> build2(ref<store_t> eval_store, ref<store_t> store,
                                                        Realise mode,
                                                        const Installables& installables,
                                                        BuildMode bMode = bmNormal);

  static void throwBuildErrors(std::vector<InstallableWithBuildResult>& build_results,
                               const store_t& store);

  static std::set<store_path_t> toStorePathSet(ref<store_t> eval_store, ref<store_t> store,
                                               Realise mode, OperateOn operateOn,
                                               const Installables& installables);

  static std::vector<store_path_t> toStorePaths(ref<store_t> eval_store, ref<store_t> store,
                                                Realise mode, OperateOn operateOn,
                                                const Installables& installables);

  static store_path_t toStorePath(ref<store_t> eval_store, ref<store_t> store, Realise mode,
                                  OperateOn operateOn, ref<Installable> installable);

  static std::set<store_path_t> toDerivations(ref<store_t> store, const Installables& installables,
                                              bool useDeriver = false);

  static BuiltPaths to_built_paths(ref<store_t> eval_store, ref<store_t> store, Realise mode,
                                   OperateOn operateOn, const Installables& installables);
};

} // namespace nix
