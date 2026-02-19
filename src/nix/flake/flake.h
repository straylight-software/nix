#pragma once
///@file

#include "nix/expr/eval-cache.h"
#include "nix/expr/value.h"
#include "nix/flake/flakeref.h"
#include "nix/flake/lockfile.h"
#include "nix/util/types.h"

namespace nix {

class EvalState;

namespace flake {

struct settings_t;

struct FlakeInput;

using FlakeInputs = std::map<FlakeId, FlakeInput>;

/**
 * FlakeInput is the 'Flake'-level parsed form of the "input" entries
 * in the flake file.
 *
 * A FlakeInput is normally constructed by the 'parseFlakeInput'
 * function which parses the input specification in the '.flake' file
 * to create a 'FlakeRef' (a fetcher, the fetcher-specific
 * representation of the input specification, and possibly the fetched
 * local store path result) and then creating this FlakeInput to hold
 * that FlakeRef, along with anything that might override that
 * FlakeRef (like command-line overrides or "follows" specifications).
 *
 * A FlakeInput is also sometimes constructed directly from a FlakeRef
 * instead of starting at the flake-file input specification
 * (e.g. overrides, follows, and implicit inputs).
 *
 * A FlakeInput will usually have one of either "ref" or "follows"
 * set.  If not otherwise specified, a "ref" will be generated to a
 * 'type="indirect"' flake, which is treated as simply the name of a
 * flake to be resolved in the registry.
 */

struct FlakeInput {
  std::optional<FlakeRef> ref;

  /**
   * Whether to call the `flake.nix` file in this input to get its outputs.
   */
  bool is_flake = true;

  /**
   * Whether to fetch this input at evaluation time or at build
   * time.
   */
  bool buildTime = false;

  std::optional<InputAttrPath> follows;
  FlakeInputs overrides;
};

struct ConfigFile {
  using ConfigValue = std::variant<std::string, int64_t, Explicit<bool>, std::vector<std::string>>;

  std::map<std::string, ConfigValue> settings;

  void apply(const settings_t& settings);
};

/**
 * A flake in context
 */
struct Flake {
  /**
   * The original flake specification (by the user)
   */
  FlakeRef original_ref;

  /**
   * registry references and caching resolved to the specific underlying flake
   */
  FlakeRef resolved_ref;

  /**
   * the specific local store result of invoking the fetcher
   */
  FlakeRef locked_ref;

  /**
   * The path of `flake.nix`.
   */
  source_path_t path;

  /**
   * Pretend that `locked_ref` is dirty.
   */
  bool force_dirty = false;

  std::optional<std::string> description;

  FlakeInputs inputs;

  /**
   * Attributes to be retroactively applied to the `self` input
   * (such as `submodules = true`).
   */
  fetchers::Attrs selfAttrs;

  /**
   * 'nixConfig' attribute
   */
  ConfigFile config;

  ~Flake();

  source_path_t lock_file_path() { return path.parent() / "flake.lock"; }
};

Flake get_flake(EvalState& state, const FlakeRef& flake_ref, fetchers::UseRegistries use_registries,
               bool require_lockable = true);

/**
 * Fingerprint of a locked flake; used as a cache key.
 */
using Fingerprint = Hash;

struct LockedFlake {
  Flake flake;
  LockFile lock_file;

  /**
   * Source tree accessors for nodes that have been fetched in
   * lock_flake(); in particular, the root node and the overridden
   * inputs.
   */
  std::map<ref<Node>, source_path_t> nodePaths;

  std::optional<Fingerprint> get_fingerprint(Store& store,
                                            const fetchers::settings_t& fetch_settings) const;
};

struct LockFlags {
  /**
   * Whether to ignore the existing lock file, creating a new one
   * from scratch.
   */
  bool recreateLockFile = false;

  /**
   * Whether to update the lock file at all. If set to false, if any
   * change to the lock file is needed (e.g. when an input has been
   * added to flake.nix), you get a fatal error.
   */
  bool updateLockFile = true;

  /**
   * Whether to write the lock file to disk. If set to true, if the
   * any changes to the lock file are needed and the flake is not
   * writable (i.e. is not a local git working tree or similar), you
   * get a fatal error. If set to false, Nix will use the modified
   * lock file in memory only, without writing it to disk.
   */
  bool writeLockFile = true;

  /**
   * Throw an exception when the flake has an unlocked input.
   */
  bool failOnUnlocked = false;

  /**
   * Whether to use the registries to lookup indirect flake
   * references like 'nixpkgs'.
   */
  std::optional<bool> use_registries = std::nullopt;

  /**
   * Whether to apply flake's nixConfig attribute to the configuration
   */

  bool applyNixConfig = false;

  /**
   * Whether unlocked flake references (i.e. those without a git
   * revision or similar) without a corresponding lock are
   * allowed. Unlocked flake references with a lock are always
   * allowed.
   */
  bool allowUnlocked = true;

  /**
   * Whether to commit changes to flake.lock.
   */
  bool commitLockFile = false;

  /**
   * The path to a lock file to read instead of the `flake.lock` file in the top-level flake
   */
  std::optional<source_path_t> referenceLockFilePath;

  /**
   * The path to a lock file to write to instead of the `flake.lock` file in the top-level flake
   */
  std::optional<std::filesystem::path> output_lock_file_path;

  /**
   * Flake inputs to be overridden.
   */
  std::map<InputAttrPath, FlakeRef> inputOverrides;

  /**
   * Flake inputs to be updated. This means that any existing lock
   * for those inputs will be ignored.
   */
  std::set<InputAttrPath> inputUpdates;

  /**
   * Whether to require a locked input.
   */
  bool require_lockable = true;
};

LockedFlake lock_flake(const settings_t& settings, EvalState& state, const FlakeRef& flake_ref,
                      const LockFlags& lock_flags);

void call_flake(EvalState& state, const LockedFlake& locked_flake, Value& v);

/**
 * Open an evaluation cache for a flake.
 */
ref<eval_cache::EvalCache> open_eval_cache(EvalState& state, ref<const LockedFlake> locked_flake);

} // namespace flake

void emit_tree_attrs(EvalState& state, const StorePath& store_path, const fetchers::Input& input,
                   Value& v, bool empty_rev_fallback = false, bool force_dirty = false);

} // namespace nix
