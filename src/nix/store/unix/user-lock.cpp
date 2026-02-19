#include "nix/store/user-lock.h"

#include <vector>

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
  auto_close_fd_t fdUserLock;
  uid_t uid;
  gid_t gid;
  std::vector<gid_t> supplementaryGIDs;

  uid_t getUID() override {
    assert(uid);
    return uid;
  }

  uid_t getUIDCount() override { return 1; }

  gid_t getGID() override {
    assert(gid);
    return gid;
  }

  std::vector<gid_t> getSupplementaryGIDs() override { return supplementaryGIDs; }

  static std::unique_ptr<UserLock> acquire() {
    assert(settings.buildUsersGroup != "");
    createDirs(settings.nixStateDir + "/userpool");

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

      auto fnUserLock = fmt("%s/userpool/%s", settings.nixStateDir, pw->pw_uid);

      auto_close_fd_t fd = open(fnUserLock.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
      if (!fd)
        throw sys_error_t("opening user lock '%s'", fnUserLock);

      if (lockFile(fd.get(), ltWrite, false)) {
        auto lock = std::make_unique<simple_user_lock_t>();

        lock->fdUserLock = std::move(fd);
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
            lock->supplementaryGIDs.push_back(gid);
        }
#endif

        return lock;
      }
    }

    return nullptr;
  }
};

struct auto_user_lock_t : UserLock {
  auto_close_fd_t fdUserLock;
  uid_t firstUid = 0;
  gid_t firstGid = 0;
  uid_t nrIds = 1;

  uid_t getUID() override {
    assert(firstUid);
    return firstUid;
  }

  gid_t getUIDCount() override { return nrIds; }

  gid_t getGID() override {
    assert(firstGid);
    return firstGid;
  }

  std::vector<gid_t> getSupplementaryGIDs() override { return {}; }

  static std::unique_ptr<UserLock> acquire(uid_t nrIds, bool useUserNamespace) {
#if !defined(__linux__)
    useUserNamespace = false;
#endif

    experimentalFeatureSettings.require(xp_t::AutoAllocateUids);
    assert(settings.startId > 0);
    assert(settings.uidCount % maxIdsPerBuild == 0);
    assert((uint64_t)settings.startId + (uint64_t)settings.uidCount <=
           std::numeric_limits<uid_t>::max());
    assert(nrIds <= maxIdsPerBuild);

    createDirs(settings.nixStateDir + "/userpool2");

    size_t nrSlots = settings.uidCount / maxIdsPerBuild;

    for (size_t i = 0; i < nrSlots; i++) {
      debug("trying user slot '%d'", i);

      createDirs(settings.nixStateDir + "/userpool2");

      auto fnUserLock = fmt("%s/userpool2/slot-%d", settings.nixStateDir, i);

      auto_close_fd_t fd = open(fnUserLock.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
      if (!fd)
        throw sys_error_t("opening user lock '%s'", fnUserLock);

      if (lockFile(fd.get(), ltWrite, false)) {
        auto firstUid = settings.startId + i * maxIdsPerBuild;

        auto pw = getpwuid(firstUid);
        if (pw)
          throw Error("auto-allocated UID %d clashes with existing user account '%s'", firstUid,
                      pw->pw_name);

        auto lock = std::make_unique<auto_user_lock_t>();
        lock->fdUserLock = std::move(fd);
        lock->firstUid = firstUid;
        if (useUserNamespace)
          lock->firstGid = firstUid;
        else {
          struct group* gr = getgrnam(settings.buildUsersGroup.get().c_str());
          if (!gr)
            throw Error("the group '%s' specified in 'build-users-group' does not exist",
                        settings.buildUsersGroup);
          lock->firstGid = gr->gr_gid;
        }
        lock->nrIds = nrIds;
        return lock;
      }
    }

    return nullptr;
  }
};

std::unique_ptr<UserLock> acquireUserLock(uid_t nrIds, bool useUserNamespace) {
  if (settings.autoAllocateUids)
    return auto_user_lock_t::acquire(nrIds, useUserNamespace);
  else
    return simple_user_lock_t::acquire();
}

bool useBuildUsers() {
#ifdef __linux__
  static bool b = (settings.buildUsersGroup != "" || settings.autoAllocateUids) && isRootUser();
  return b;
#elif defined(__APPLE__) || defined(__FreeBSD__)
  static bool b = settings.buildUsersGroup != "" && isRootUser();
  return b;
#else
  return false;
#endif
}

} // namespace nix
