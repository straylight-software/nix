#pragma once
/**
 * @file
 *
 * Implementation of Profiles.
 *
 * See the manual for additional information.
 */

#include <filesystem>
#include <optional>

#include <time.h>

#include "nix/store/pathlocks.h"
#include "nix/util/types.h"

namespace nix {

class store_path_t;

/**
 * A positive number identifying a generation for a given profile.
 *
 * Generation numbers are assigned sequentially. Each new generation is
 * assigned 1 + the current highest generation number.
 */
using GenerationNumber = uint64_t;

/**
 * A generation is a revision of a profile.
 *
 * Each generation is a mapping (key-value pair) from an identifier
 * (`number`) to a store object (specified by `path`).
 */
struct Generation {
  /**
   * The number of a generation is its unique identifier within the
   * profile.
   */
  GenerationNumber number;
  /**
   * The store path identifies the store object that is the contents
   * of the generation.
   *
   * These store paths / objects are not unique to the generation
   * within a profile. Nix tries to ensure successive generations have
   * distinct contents to avoid bloat, but nothing stops two
   * non-adjacent generations from having the same contents.
   *
   * @todo use `store_path_t` instead of `std::filesystem::path`?
   */
  std::filesystem::path path;

  /**
   * When the generation was created. This is extra metadata about the
   * generation used to make garbage collecting old generations more
   * convenient.
   */
  time_t creationTime;
};

/**
 * All the generations of a profile
 */
using Generations = std::list<Generation>;

/**
 * Find all generations for the given profile.
 *
 * @param profile A profile specified by its name and location combined
 * into a path. E.g. if "foo" is the name of the profile, and "/bar/baz"
 * is the directory it is in, then the path "/bar/baz/foo" would be the
 * argument for this parameter.
 *
 * @return The pair of:
 *
 *   - The list of currently present generations for the specified profile,
 *     sorted by ascending generation number.
 *
 *   - The number of the current/active generation.
 *
 * Note that the current/active generation need not be the latest one.
 */
std::pair<Generations, std::optional<GenerationNumber>>
findGenerations(std::filesystem::path profile);

struct local_fs_store;

/**
 * Create a new generation of the given profile
 *
 * If the previous generation (not the currently active one!) has a
 * distinct store object, a fresh generation number is mapped to the
 * given store object, referenced by path. Otherwise, the previous
 * generation is assumed.
 *
 * The behavior of reusing existing generations like this makes this
 * procedure idempotent. It also avoids clutter.
 */
std::filesystem::path create_generation(local_fs_store& store, std::filesystem::path profile,
                                       store_path_t out_path);

/**
 * Unconditionally delete a generation
 *
 * @param profile A profile specified by its name and location combined into a path.
 *
 * @param gen The generation number specifying exactly which generation
 * to delete.
 *
 * Because there is no check of whether the generation to delete is
 * active, this is somewhat unsafe.
 *
 * @todo Should we expose this at all?
 */
void delete_generation(const std::filesystem::path& profile, GenerationNumber gen);

/**
 * Delete the given set of generations.
 *
 * @param profile The profile, specified by its name and location combined into a path, whose
 * generations we want to delete.
 *
 * @param gens_to_delete The generations to delete, specified by a set of
 * numbers.
 *
 * @param dry_run Log what would be deleted instead of actually doing
 * so.
 *
 * Trying to delete the currently active generation will fail, and cause
 * no generations to be deleted.
 */
void delete_generations(const std::filesystem::path& profile,
                       const std::set<GenerationNumber>& gens_to_delete, bool dry_run);

/**
 * Delete generations older than `max` passed the current generation.
 *
 * @param profile The profile, specified by its name and location combined into a path, whose
 * generations we want to delete.
 *
 * @param max How many generations to keep up to the current one. Must
 * be at least 1 so we don't delete the current one.
 *
 * @param dry_run Log what would be deleted instead of actually doing
 * so.
 */
void delete_generations_greater_than(const std::filesystem::path& profile, GenerationNumber max,
                                  bool dry_run);

/**
 * Delete all generations other than the current one
 *
 * @param profile The profile, specified by its name and location combined into a path, whose
 * generations we want to delete.
 *
 * @param dry_run Log what would be deleted instead of actually doing
 * so.
 */
void delete_old_generations(const std::filesystem::path& profile, bool dry_run);

/**
 * Delete generations older than `t`, except for the most recent one
 * older than `t`.
 *
 * @param profile The profile, specified by its name and location combined into a path, whose
 * generations we want to delete.
 *
 * @param dry_run Log what would be deleted instead of actually doing
 * so.
 */
void delete_generations_older_than(const std::filesystem::path& profile, time_t t, bool dry_run);

/**
 * Parse a temp spec intended for `delete_generations_older_than()`.
 *
 * Throws an exception if `time_spec` fails to parse.
 */
time_t parse_older_than_time_spec(std::string_view time_spec);

/**
 * Smaller wrapper around `replace_symlink` for replacing the current
 * generation of a profile. Does not enforce proper structure.
 *
 * @todo always use `switch_generation()` instead, and delete this.
 */
void switch_link(std::filesystem::path link, std::filesystem::path target);

/**
 * Roll back a profile to the specified generation, or to the most
 * recent one older than the current.
 */
void switch_generation(const std::filesystem::path& profile, std::optional<GenerationNumber> dst_gen,
                      bool dry_run);

/**
 * Ensure exclusive access to a profile.  Any command that modifies
 * the profile first acquires this lock.
 */
void lock_profile(PathLocks& lock, const std::filesystem::path& profile);

/**
 * Optimistic locking is used by long-running operations like `nix-env
 * -i'.  Instead of acquiring the exclusive lock for the entire
 * duration of the operation, we just perform the operation
 * optimistically (without an exclusive lock), and check at the end
 * whether the profile changed while we were busy (i.e., the symlink
 * target changed).  If so, the operation is restarted.  Restarting is
 * generally cheap, since the build results are still in the Nix
 * store.  Most of the time, only the user environment has to be
 * rebuilt.
 */
std::string optimistic_lock_profile(const std::filesystem::path& profile);

/**
 * Create and return the path to a directory suitable for storing the user’s
 * profiles.
 */
std::filesystem::path profiles_dir();

/**
 * Return the path to the profile directory for root (but don't try creating it)
 */
std::filesystem::path root_profiles_dir();

/**
 * Create and return the path to the file used for storing the users's channels
 */
std::filesystem::path default_channels_dir();

/**
 * Return the path to the channel directory for root (but don't try creating it)
 */
std::filesystem::path root_channels_dir();

/**
 * Resolve the default profile (~/.nix-profile by default,
 * $XDG_STATE_HOME/nix/profile if XDG Base directory_t Support is enabled),
 * and create if doesn't exist
 */
std::filesystem::path get_default_profile();

} // namespace nix
