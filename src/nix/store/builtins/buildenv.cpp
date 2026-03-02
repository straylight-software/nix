#include "nix/store/builtins/buildenv.h"

#include <algorithm>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "nix/store/builtins.h"
#include "nix/store/derivations.h"
#include "nix/util/signals.h"

namespace nix {

RegisterBuiltinBuilder::BuiltinBuilders& RegisterBuiltinBuilder::builtinBuilders() {
  static RegisterBuiltinBuilder::BuiltinBuilders builders;
  return builders;
}

namespace {

struct State {
  std::map<Path, int> priorities;
  unsigned long symlinks = 0;
};

} // namespace

/* For each activated package, create symlinks */
static void create_links(State& state, const Path& src_dir, const Path& dst_dir, int priority) {
  directory_iterator_t src_files;

  try {
    src_files = directory_iterator_t{src_dir};
  } catch (sys_error_t& e) {
    if (e.err_no() == ENOTDIR) {
      warn("not including '%s' in the user environment because it's not a directory", src_dir);
      return;
    }
    throw;
  }

  for (const auto& ent : src_files) {
    check_interrupt();
    auto name = ent.path().filename();
    if (name.string()[0] == '.') {
      /* not matched by glob */
      continue;
    }
    auto srcFile = (std::filesystem::path{src_dir} / name).string();
    auto dstFile = (std::filesystem::path{dst_dir} / name).string();

    struct stat srcSt;
    try {
      if (stat(srcFile.c_str(), &srcSt) == -1) {
        throw sys_error_t("getting status of '%1%'", srcFile);
      }
    } catch (sys_error_t& e) {
      if (e.err_no() == ENOENT || e.err_no() == ENOTDIR) {
        warn("skipping dangling symlink '%s'", dstFile);
        continue;
      }
      throw;
    }

    /* The files below are special-cased to that they don't show
     * up in user profiles, either because they are useless, or
     * because they would cause pointless collisions (e.g., each
     * Python package brings its own
     * `$out/lib/pythonX.Y/site-packages/easy-install.pth'.)
     */
    if (has_suffix(srcFile, "/propagated-build-inputs") || has_suffix(srcFile, "/nix-support") ||
        has_suffix(srcFile, "/perllocal.pod") || has_suffix(srcFile, "/info/dir") ||
        has_suffix(srcFile, "/log") || has_suffix(srcFile, "/manifest.nix") ||
        has_suffix(srcFile, "/manifest.json")) {
      continue;
    }

    else if (S_ISDIR(srcSt.st_mode)) {
      auto dstStOpt = maybe_lstat(dstFile.c_str());
      if (dstStOpt) {
        auto& dstSt = *dstStOpt;
        if (S_ISDIR(dstSt.st_mode)) {
          create_links(state, srcFile, dstFile, priority);
          continue;
        } else if (S_ISLNK(dstSt.st_mode)) {
          auto target = canon_path(dstFile, true);
          if (!S_ISDIR(lstat(target).st_mode)) {
            throw Error("collision between '%1%' and non-directory '%2%'", srcFile, target);
          }
          if (unlink(dstFile.c_str()) == -1) {
            throw sys_error_t("unlinking '%1%'", dstFile);
          }
          if (mkdir(dstFile.c_str()
#ifndef _WIN32 // TODO abstract mkdir perms for Windows
                        ,
                    0755
#endif
                    ) == -1)
            throw sys_error_t("creating directory '%1%'", dstFile);
          create_links(state, target, dstFile, state.priorities[dstFile]);
          create_links(state, srcFile, dstFile, priority);
          continue;
        }
      }
    }

    else {
      auto dstStOpt = maybe_lstat(dstFile.c_str());
      if (dstStOpt) {
        auto& dstSt = *dstStOpt;
        if (S_ISLNK(dstSt.st_mode)) {
          auto prevPriority = state.priorities[dstFile];
          if (prevPriority == priority) {
            throw BuildEnvFileConflictError(read_link(dstFile), srcFile, priority);
          }
          if (prevPriority < priority) {
            continue;
          }
          if (unlink(dstFile.c_str()) == -1) {
            throw sys_error_t("unlinking '%1%'", dstFile);
          }
        } else if (S_ISDIR(dstSt.st_mode)) {
          throw Error("collision between non-directory '%1%' and directory '%2%'", srcFile,
                      dstFile);
        }
      }
    }

    create_symlink(srcFile, dstFile);
    state.priorities[dstFile] = priority;
    state.symlinks++;
  }
}

void build_profile(const Path& out, Packages&& pkgs) {
  State state;

  path_set_t done, postponed;

  auto add_pkg = [&](const Path& pkg_dir, int priority) {
    if (!done.insert(pkg_dir).second) {
      return;
    }
    create_links(state, pkg_dir, out, priority);

    try {
      for (const auto& p : tokenize_string<std::vector<std::string>>(
               read_file(pkg_dir + "/nix-support/propagated-user-env-packages"), " \n")) {
        if (!done.count(p)) {
          postponed.insert(p);
        }
      }
    } catch (sys_error_t& e) {
      if (e.err_no() != ENOENT && e.err_no() != ENOTDIR) {
        throw;
      }
    }
  };

  /* symlink to the packages that have been installed explicitly by the
   * user. Process in priority order to reduce unnecessary
   * symlink/unlink steps.
   */
  std::sort(pkgs.begin(), pkgs.end(), [](const Package& a, const Package& b) {
    return a.priority < b.priority || (a.priority == b.priority && a.path < b.path);
  });
  for (const auto& pkg : pkgs) {
    if (pkg.active) {
      add_pkg(pkg.path, pkg.priority);
    }
  }

  /* symlink to the packages that have been "propagated" by packages
   * installed by the user (i.e., package X declares that it wants Y
   * installed as well). We do these later because they have a lower
   * priority in case of collisions.
   */
  auto priority_counter = 1000;
  while (!postponed.empty()) {
    path_set_t pkg_dirs;
    postponed.swap(pkg_dirs);
    for (const auto& pkg_dir : pkg_dirs) {
      add_pkg(pkg_dir, priority_counter++);
    }
  }

  debug("created %d symlinks in user environment", state.symlinks);
}

static void builtin_buildenv(const BuiltinBuilderContext& ctx) {
  auto get_attr = [&](const std::string& name) {
    auto i = ctx.drv.env.find(name);
    if (i == ctx.drv.env.end()) {
      throw Error("attribute '%s' missing", name);
    }
    return i->second;
  };

  auto out = ctx.outputs.at("out");
  create_dirs(out);

  /* Convert the stuff we get from the environment back into a
   * coherent data type. */
  Packages pkgs;
  {
    auto derivations = tokenize_string<strings_t>(get_attr("derivations"));

    auto item_it = derivations.begin();
    while (item_it != derivations.end()) {
      /* !!! We're trusting the caller to structure derivations env var correctly */
      const bool active = "false" != *item_it++;
      const int priority = stoi(*item_it++);
      const size_t outputs = stoul(*item_it++);

      for (size_t n{0}; n < outputs; n++) {
        pkgs.emplace_back(std::move(*item_it++), active, priority);
      }
    }
  }

  build_profile(out, std::move(pkgs));

  create_symlink(get_attr("manifest"), out + "/manifest.nix");
}

static RegisterBuiltinBuilder register_buildenv("buildenv", builtin_buildenv);

} // namespace nix
