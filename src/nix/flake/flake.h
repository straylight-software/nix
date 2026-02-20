#pragma once
///@file

#include "nix/expr/eval-cache.h"
#include "nix/expr/value.h"
#include "nix/flake/flakeref.h"
#include "nix/flake/lockfile.h"
#include "nix/util/types.h"

namespace nix {

class eval_state_t;

namespace flake {

struct settings_t;

struct FlakeInput;

using FlakeInputs = std::map<FlakeId, FlakeInput>;

/**
 * FlakeInput is the 'flake_t'-level parsed form of the "input" entries
 * in the flake file.
 *
 * A FlakeInput is normally constructed by the 'parseFlakeInput'
 * function which parses the input specification in the '.flake' file
 * to create a 'flake_ref_t' (a fetcher, the fetcher-specific
 * representation of the input specification, and possibly the fetched
 * local store path result) and then creating this FlakeInput to hold
 * that flake_ref_t, along with anything that might override that
 * flake_ref_t (like command-line overrides or "follows" specifications).
 *
 * A FlakeInput is also sometimes constructed directly from a flake_ref_t
 * instead of starting at the flake-file input specification
 * (e.g. overrides, follows, and implicit inputs).
 *
 * A FlakeInput will usually have one of either "ref" or "follows"
 * set.  If not otherwise specified, a "ref" will be generated to a
 * 'type="indirect"' flake, which is treated as simply the name of a
 * flake to be resolved in the registry.
 */

struct FlakeInput {
  std::optional<flake_ref_t> ref;

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
  using ConfigValue = std::variant<std::string, int64_t, explicit_t<bool>, std::vector<std::string>>;

  std::map<std::string, ConfigValue> settings;

  void apply(const settings_t& settings);
};

/**
 * A flake in context
 */
struct flake_t {
  /**
   * The original flake specification (by the user)
   */
  flake_ref_t original_ref;

  /**
   * registry references and caching resolved to the specific underlying flake
   */
  flake_ref_t resolved_ref;

  /**
   * the specific local store result of invoking the fetcher
   */
  flake_ref_t locked_ref;

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

  ~flake_t();

  source_path_t lock_file_path() { return path.parent() / "flake.lock"; }
};

flake_t get_flake(eval_state_t& state, const flake_ref_t& flake_ref, fetchers::UseRegistries use_registries,
               bool require_lockable = true);

/**
 * Fingerprint of a locked flake; used as a cache key.
 */
using Fingerprint = Hash;

struct LockedFlake {
  flake_t flake;
  lock_file_t lock_file;

  /**
   * source_t tree accessors for nodes that have been fetched in
   * lock_flake(); in particular, the root node and the overridden
   * inputs.
   */
  std::map<ref<Node>, source_path_t> nodePaths;

  std::optional<Fingerprint> get_fingerprint(store_t& store,
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
   * flake_t inputs to be overridden.
   */
  std::map<InputAttrPath, flake_ref_t> inputOverrides;

  /**
   * flake_t inputs to be updated. This means that any existing lock
   * for those inputs will be ignored.
   */
  std::set<InputAttrPath> inputUpdates;

  /**
   * Whether to require a locked input.
   */
  bool require_lockable = true;
};

LockedFlake lock_flake(const settings_t& settings, eval_state_t& state, const flake_ref_t& flake_ref,
                      const LockFlags& lock_flags);

void call_flake(eval_state_t& state, const LockedFlake& locked_flake, value_t& v);

/**
 * Open an evaluation cache for a flake.
 */
ref<eval_cache::EvalCache> open_eval_cache(eval_state_t& state, ref<const LockedFlake> locked_flake);

} // namespace flake

void emit_tree_attrs(eval_state_t& state, const store_path_t& store_path, const fetchers::input_t& input,
                   value_t& v, bool empty_rev_fallback = false, bool force_dirty = false);

} // namespace nix
