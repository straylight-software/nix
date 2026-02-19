#include "nix/store/user-lock.h"

#include <vector>

#include <fcntl.h>
#include <grp.h>
#include <pwd.h>

#include "nix/store/globals.h"
#include "nix/store/pathlocks.h"
#include "nix/util/file-system.h"
#include "nix/util/logging.h"
#include "nix/util/users.h"

namespace nix {

#ifdef __linux__

static std::vector<gid_t> get_group_list(const char* username, gid_t group_id) {
  std::vector<gid_t> gids;
  gids.resize(32); // Initial guess

  auto getgroupl_failed{[&] {
    int ngroups = gids.size();
    int err = getgrouplist(username, group_id, gids.data(), &ngroups);
    gids.resize(ngroups);
    return err == -1;
  }};

  // The first error means that the vector was not big enough.
  // If it happens again, there is some different problem.
  if (getgroupl_failed() && getgroupl_failed()) {
    throw sys_error_t("failed to get list of supplementary groups for '%s'", username);
  }

  return gids;
}
#endif

struct simple_user_lock_t : UserLock {
  auto_close_fd_t fd_user_lock;
  uid_t uid;
  gid_t gid;
  std::vector<gid_t> supplementary_gi_ds;

  uid_t getUID() override {
    assert(uid);
    return uid;
  }

  uid_t getUIDCount() override { return 1; }

  gid_t getGID() override {
    assert(gid);
    return gid;
  }

  std::vector<gid_t> getSupplementaryGIDs() override { return supplementary_gi_ds; }

  static std::unique_ptr<UserLock> acquire() {
    assert(settings.buildUsersGroup != "");
    create_dirs(settings.nixStateDir + "/userpool");

    /* Get the members of the build-users-group. */
    struct group* gr = getgrnam(settings.buildUsersGroup.get().c_str());
    if (!gr)
      throw Error("the group '%s' specified in 'build-users-group' does not exist",
                  settings.buildUsersGroup);

    /* Copy the result of getgrnam. */
    strings_t users;
    for (char** p = gr->gr_mem; *p; ++p) {
      debug("found build user '%s'", *p);
      users.push_back(*p);
    }

    if (users.empty())
      throw Error("the build users group '%s' has no members", settings.buildUsersGroup);

    /* Find a user account that isn't currently in use for another
       build. */
    for (auto& i : users) {
      debug("trying user '%s'", i);

      struct passwd* pw = getpwnam(i.c_str());
      if (!pw)
        throw Error("the user '%s' in the group '%s' does not exist", i, settings.buildUsersGroup);

      auto fn_user_lock = fmt("%s/userpool/%s", settings.nixStateDir, pw->pw_uid);

      auto_close_fd_t fd = open(fn_user_lock.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
      if (!fd)
        throw sys_error_t("opening user lock '%s'", fn_user_lock);

      if (lock_file(fd.get(), ltWrite, false)) {
        auto lock = std::make_unique<simple_user_lock_t>();

        lock->fd_user_lock = std::move(fd);
        lock->uid = pw->pw_uid;
        lock->gid = gr->gr_gid;

        /* Sanity check... */
        if (lock->uid == getuid() || lock->uid == geteuid())
          throw Error("the Nix user should not be a member of '%s'", settings.buildUsersGroup);

#ifdef __linux__
        /* Get the list of supplementary groups of this user. This is
         * usually either empty or contains a group such as "kvm". */

        // Finally, trim back the GID list to its real size.
        for (auto gid : get_group_list(pw->pw_name, pw->pw_gid)) {
          if (gid != lock->gid)
            lock->supplementary_gi_ds.push_back(gid);
        }
#endif

        return lock;
      }
    }

    return nullptr;
  }
};

struct auto_user_lock_t : UserLock {
  auto_close_fd_t fd_user_lock;
  uid_t first_uid = 0;
  gid_t first_gid = 0;
  uid_t nr_ids = 1;

  uid_t getUID() override {
    assert(first_uid);
    return first_uid;
  }

  gid_t getUIDCount() override { return nr_ids; }

  gid_t getGID() override {
    assert(first_gid);
    return first_gid;
  }

  std::vector<gid_t> getSupplementaryGIDs() override { return {}; }

  static std::unique_ptr<UserLock> acquire(uid_t nr_ids, bool use_user_namespace) {
#if !defined(__linux__)
    use_user_namespace = false;
#endif

    experimental_feature_settings.require(xp_t::auto_allocate_uids);
    assert(settings.startId > 0);
    assert(settings.uidCount % maxIdsPerBuild == 0);
    assert((uint64_t)settings.startId + (uint64_t)settings.uidCount <=
           std::numeric_limits<uid_t>::max());
    assert(nr_ids <= maxIdsPerBuild);

    create_dirs(settings.nixStateDir + "/userpool2");

    size_t nr_slots = settings.uidCount / maxIdsPerBuild;

    for (size_t i = 0; i < nr_slots; i++) {
      debug("trying user slot '%d'", i);

      create_dirs(settings.nixStateDir + "/userpool2");

      auto fn_user_lock = fmt("%s/userpool2/slot-%d", settings.nixStateDir, i);

      auto_close_fd_t fd = open(fn_user_lock.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
      if (!fd)
        throw sys_error_t("opening user lock '%s'", fn_user_lock);

      if (lock_file(fd.get(), ltWrite, false)) {
        auto first_uid = settings.startId + i * maxIdsPerBuild;

        auto pw = getpwuid(first_uid);
        if (pw)
          throw Error("auto-allocated UID %d clashes with existing user account '%s'", first_uid,
                      pw->pw_name);

        auto lock = std::make_unique<auto_user_lock_t>();
        lock->fd_user_lock = std::move(fd);
        lock->first_uid = first_uid;
        if (use_user_namespace)
          lock->first_gid = first_uid;
        else {
          struct group* gr = getgrnam(settings.buildUsersGroup.get().c_str());
          if (!gr)
            throw Error("the group '%s' specified in 'build-users-group' does not exist",
                        settings.buildUsersGroup);
          lock->first_gid = gr->gr_gid;
        }
        lock->nr_ids = nr_ids;
        return lock;
      }
    }

    return nullptr;
  }
};

std::unique_ptr<UserLock> acquire_user_lock(uid_t nr_ids, bool use_user_namespace) {
  if (settings.autoAllocateUids)
    return auto_user_lock_t::acquire(nr_ids, use_user_namespace);
  else
    return simple_user_lock_t::acquire();
}

bool use_build_users() {
#ifdef __linux__
  static bool b = (settings.buildUsersGroup != "" || settings.autoAllocateUids) && is_root_user();
  return b;
#elif defined(__APPLE__) || defined(__FreeBSD__)
  static bool b = settings.buildUsersGroup != "" && is_root_user();
  return b;
#else
  return false;
#endif
}

} // namespace nix
