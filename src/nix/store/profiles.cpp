#include "nix/store/profiles.h"

#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "nix/store/globals.h"
#include "nix/store/local-fs-store.h"
#include "nix/store/store-api.h"
#include "nix/util/signals.h"
#include "nix/util/users.h"

namespace nix {

/**
 * Parse a generation name of the format
 * `<profilename>-<number>-link'.
 */
static std::optional<GenerationNumber> parse_name(const std::string& profile_name,
                                                  const std::string& name) {
  if (name.substr(0, profile_name.size() + 1) != profile_name + "-") {
    return {};
  }
  auto s = name.substr(profile_name.size() + 1);
  auto p = s.find("-link");
  if (p == std::string::npos) {
    return {};
  }
  if (auto n = string2_int<unsigned int>(s.substr(0, p))) {
    return *n;
  } else {
    return {};
  }
}

std::pair<Generations, std::optional<GenerationNumber>>
findGenerations(std::filesystem::path profile) {
  Generations gens;

  std::filesystem::path profileDir = profile.parent_path();
  auto profile_name = profile.filename().string();

  for (auto& i : directory_iterator_t{profileDir}) {
    check_interrupt();
    if (auto n = parse_name(profile_name, i.path().filename().string())) {
      auto path = i.path().string();
      gens.push_back({.number = *n, .path = path, .creationTime = lstat(path).st_mtime});
    }
  }

  gens.sort([](const Generation& a, const Generation& b) { return a.number < b.number; });

  return {gens, path_exists(profile) ? parse_name(profile_name, read_link(profile).string())
                                     : std::nullopt};
}

/**
 * Create a generation name that can be parsed by `parse_name()`.
 */
static std::filesystem::path make_name(const std::filesystem::path& profile, GenerationNumber num) {
  /* NB std::filesystem::path when put in format strings is
     quoted automatically. */
  return fmt("%s-%s-link", profile.string(), num);
}

std::filesystem::path create_generation(local_fs_store& store, std::filesystem::path profile,
                                        store_path_t out_path) {
  /* The new generation number should be higher than old the
     previous ones. */
  auto [gens, dummy] = findGenerations(profile);

  GenerationNumber num;
  if (gens.size() > 0) {
    Generation last = gens.back();

    if (read_link(last.path) == store.printStorePath(out_path)) {
      /* We only create a new generation symlink if it differs
         from the last one.

         This helps keeping gratuitous installs/rebuilds from piling
         up uncontrolled numbers of generations, cluttering up the
         UI like grub. */
      return last.path;
    }

    num = last.number;
  } else {
    num = 0;
  }

  /* Create the new generation.  Note that addPermRoot() blocks if
     the garbage collector is running to prevent the stuff we've
     built from moving from the temporary roots (which the GC knows)
     to the permanent roots (of which the GC would have a stale
     view).  If we didn't do it this way, the GC might remove the
     user environment etc. we've just built. */
  auto generation = make_name(profile, num + 1);
  store.addPermRoot(out_path, generation.string());

  return generation;
}

static void remove_file(const std::filesystem::path& path) {
  try {
    std::filesystem::remove(path);
  } catch (std::filesystem::filesystem_error& e) {
    throw sys_error_t("removing file '%1%'", path);
  }
}

void delete_generation(const std::filesystem::path& profile, GenerationNumber gen) {
  std::filesystem::path generation = make_name(profile, gen);
  remove_file(generation);
}

/**
 * Delete a generation with dry-run mode.
 *
 * Like `delete_generation()` but:
 *
 *  - We log what we are going to do.
 *
 *  - We only actually delete if `dry_run` is false.
 */
static void delete_generation2(const std::filesystem::path& profile, GenerationNumber gen,
                               bool dry_run) {
  if (dry_run) {
    notice("would remove profile version %1%", gen);
  } else {
    notice("removing profile version %1%", gen);
    delete_generation(profile, gen);
  }
}

void delete_generations(const std::filesystem::path& profile,
                        const std::set<GenerationNumber>& gens_to_delete, bool dry_run) {
  PathLocks lock;
  lock_profile(lock, profile);

  auto [gens, cur_gen] = findGenerations(profile);

  if (gens_to_delete.count(*cur_gen)) {
    throw Error("cannot delete current version of profile %1%'", profile);
  }

  for (auto& i : gens) {
    if (!gens_to_delete.count(i.number)) {
      continue;
    }
    delete_generation2(profile, i.number, dry_run);
  }
}

/**
 * Advanced the iterator until the given predicate `cond` returns `true`.
 */
static inline void iter_drop_until(Generations& gens, auto&& i, auto&& cond) {
  for (; i != gens.rend() && !cond(*i); ++i)
    ;
}

void delete_generations_greater_than(const std::filesystem::path& profile, GenerationNumber max,
                                     bool dry_run) {
  if (max == 0) {
    throw Error("Must keep at least one generation, otherwise the current one would be deleted");
  }

  PathLocks lock;
  lock_profile(lock, profile);

  auto [gens, _curGen] = findGenerations(profile);
  auto cur_gen = _curGen;

  auto i = gens.rbegin();

  // Find the current generation
  iter_drop_until(gens, i, [&](auto& g) { return g.number == cur_gen; });

  // Skip over `max` generations, preserving them
  for (GenerationNumber keep = 0; i != gens.rend() && keep < max; ++i, ++keep)
    ;

  // Delete the rest
  for (; i != gens.rend(); ++i) {
    delete_generation2(profile, i->number, dry_run);
  }
}

void delete_old_generations(const std::filesystem::path& profile, bool dry_run) {
  PathLocks lock;
  lock_profile(lock, profile);

  auto [gens, cur_gen] = findGenerations(profile);

  for (auto& i : gens) {
    if (i.number != cur_gen) {
      delete_generation2(profile, i.number, dry_run);
    }
  }
}

void delete_generations_older_than(const std::filesystem::path& profile, time_t t, bool dry_run) {
  PathLocks lock;
  lock_profile(lock, profile);

  auto [gens, cur_gen] = findGenerations(profile);

  auto i = gens.rbegin();

  // Predicate that the generation is older than the given time.
  auto older = [&](auto& g) { return g.creationTime < t; };

  // Find the first older generation, if one exists
  iter_drop_until(gens, i, older);

  /* Take the previous generation

     We don't want delete this one yet because it
     existed at the requested point in time, and
     we want to be able to roll back to it. */
  if (i != gens.rend()) {
    ++i;
  }

  // Delete all previous generations (unless current).
  for (; i != gens.rend(); ++i) {
    /* Creating date and generations should be monotonic, so lower
       numbered derivations should also be older. */
    assert(older(*i));
    if (i->number != cur_gen) {
      delete_generation2(profile, i->number, dry_run);
    }
  }
}

time_t parse_older_than_time_spec(std::string_view time_spec) {
  if (time_spec.empty() || time_spec[time_spec.size() - 1] != 'd') {
    throw UsageError("invalid number of days specifier '%1%', expected something like '14d'",
                     time_spec);
  }

  time_t cur_time = time(0);
  auto str_days = time_spec.substr(0, time_spec.size() - 1);
  auto days = string2_int<int>(str_days);

  if (!days || *days < 1) {
    throw UsageError("invalid number of days specifier '%1%'", time_spec);
  }

  return cur_time - *days * 24 * 3600;
}

void switch_link(std::filesystem::path link, std::filesystem::path target) {
  /* Hacky. */
  if (target.parent_path() == link.parent_path()) {
    target = target.filename();
  }

  replace_symlink(target, link);
}

void switch_generation(const std::filesystem::path& profile,
                       std::optional<GenerationNumber> dst_gen, bool dry_run) {
  PathLocks lock;
  lock_profile(lock, profile);

  auto [gens, cur_gen] = findGenerations(profile);

  std::optional<Generation> dst;
  for (auto& i : gens) {
    if ((!dst_gen && i.number < cur_gen) || (dst_gen && i.number == *dst_gen)) {
      dst = i;
    }
  }

  if (!dst) {
    if (dst_gen) {
      throw Error("profile version %1% does not exist", *dst_gen);
    } else {
      throw Error("no profile version older than the current (%1%) exists", cur_gen.value_or(0));
    }
  }

  notice("switching profile from version %d to %d", cur_gen.value_or(0), dst->number);

  if (dry_run) {
    return;
  }

  switch_link(profile, dst->path);
}

void lock_profile(PathLocks& lock, const std::filesystem::path& profile) {
  lock.lockPaths({profile}, fmt("waiting for lock on profile '%1%'", profile));
  lock.setDeletion(true);
}

std::string optimistic_lock_profile(const std::filesystem::path& profile) {
  return path_exists(profile) ? read_link(profile).string() : "";
}

std::filesystem::path profiles_dir() {
  auto profile_root = is_root_user() ? root_profiles_dir()
                                     : std::filesystem::path{create_nix_state_dir()} / "profiles";
  create_dirs(profile_root);
  return profile_root;
}

std::filesystem::path root_profiles_dir() {
  return std::filesystem::path{settings.nixStateDir} / "profiles/per-user/root";
}

std::filesystem::path get_default_profile() {
  std::filesystem::path profile_link =
      settings.useXDGBaseDirectories ? std::filesystem::path{create_nix_state_dir()} / "profile"
                                     : std::filesystem::path{get_home()} / ".nix-profile";
  try {
    auto profile = profiles_dir() / "profile";
    if (!path_exists(profile_link)) {
      replace_symlink(profile, profile_link);
    }
    // Backwards compatibility measure: Make root's profile available as
    // `.../default` as it's what NixOS and most of the init scripts expect
    auto global_profile_link = std::filesystem::path{settings.nixStateDir} / "profiles" / "default";
    if (is_root_user() && !path_exists(global_profile_link)) {
      replace_symlink(profile, global_profile_link);
    }
    auto link_dir = profile_link.parent_path();
    return abs_path(read_link(profile_link), &link_dir);
  } catch (Error&) {
    return profile_link;
  } catch (std::filesystem::filesystem_error&) {
    return profile_link;
  }
}

std::filesystem::path default_channels_dir() {
  return profiles_dir() / "channels";
}

std::filesystem::path root_channels_dir() {
  return root_profiles_dir() / "channels";
}

} // namespace nix
