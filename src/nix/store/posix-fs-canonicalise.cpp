#include "nix/store/posix-fs-canonicalise.h"

#include "nix/store/globals.h"
#include "nix/store/store-api.h"
#include "nix/util/file-system.h"
#include "nix/util/signals.h"
#include "nix/util/util.h"
#include "store-config-private.h"

#if NIX_SUPPORT_ACL
#  include <sys/xattr.h>
#endif

namespace nix {

const time_t mtime_store = 1; /* 1 second into the epoch */

static void canonicalise_timestamp_and_permissions(const Path& path, const struct stat& st) {
  if (!S_ISLNK(st.st_mode)) {
    /* Mask out all type related bits. */
    mode_t mode = st.st_mode & ~S_IFMT;
    bool is_dir = S_ISDIR(st.st_mode);
    if ((mode != 0444 || is_dir) && mode != 0555) {
      mode = (st.st_mode & S_IFMT) | 0444 | (st.st_mode & S_IXUSR || is_dir ? 0111 : 0);
      if (chmod(path.c_str(), mode) == -1)
        throw sys_error_t("changing mode of '%1%' to %2$o", path, mode);
    }
  }

#ifndef _WIN32 // TODO implement
  if (st.st_mtime != mtime_store) {
    struct stat st2 = st;
    st2.st_mtime = mtime_store, set_write_time(path, st2);
  }
#endif
}

void canonicalise_timestamp_and_permissions(const Path& path) {
  canonicalise_timestamp_and_permissions(path, lstat(path));
}

static void canonicalise_path_meta_data_(const Path& path,
#ifndef _WIN32
                                      std::optional<std::pair<uid_t, uid_t>> uidRange,
#endif
                                      InodesSeen& inodes_seen) {
  check_interrupt();

#ifdef __APPLE__
  /* Remove flags, in particular UF_IMMUTABLE which would prevent
     the file from being garbage-collected. FIXME: use
     setattrlist() to remove other attributes as well. */
  if (lchflags(path.c_str(), 0)) {
    if (errno != ENOTSUP)
      throw sys_error_t("clearing flags of path '%1%'", path);
  }
#endif

  auto st = lstat(path);

  /* Really make sure that the path is of a supported type. */
  if (!(S_ISREG(st.st_mode) || S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)))
    throw Error("file '%1%' has an unsupported type", path);

#if NIX_SUPPORT_ACL
  /* Remove extended attributes / ACLs. */
  ssize_t eaSize = llistxattr(path.c_str(), nullptr, 0);

  if (eaSize < 0) {
    if (errno != ENOTSUP && errno != ENODATA)
      throw sys_error_t("querying extended attributes of '%s'", path);
  } else if (eaSize > 0) {
    std::vector<char> eaBuf(eaSize);

    if ((eaSize = llistxattr(path.c_str(), eaBuf.data(), eaBuf.size())) < 0)
      throw sys_error_t("querying extended attributes of '%s'", path);

    for (auto& eaName :
         tokenize_string<strings_t>(std::string(eaBuf.data(), eaSize), std::string("\000", 1))) {
      if (settings.ignoredAcls.get().count(eaName))
        continue;
      if (lremovexattr(path.c_str(), eaName.c_str()) == -1)
        throw sys_error_t("removing extended attribute '%s' from '%s'", eaName, path);
    }
  }
#endif

#ifndef _WIN32
  /* Fail if the file is not owned by the build user.  This prevents
     us from messing up the ownership/permissions of files
     hard-linked into the output (e.g. "ln /etc/shadow $out/foo").
     However, ignore files that we chown'ed ourselves previously to
     ensure that we don't fail on hard links within the same build
     (i.e. "touch $out/foo; ln $out/foo $out/bar"). */
  if (uidRange && (st.st_uid < uidRange->first || st.st_uid > uidRange->second)) {
    if (S_ISDIR(st.st_mode) || !inodes_seen.count(Inode(st.st_dev, st.st_ino)))
      throw BuildError(BuildResult::Failure::OutputRejected, "invalid ownership on file '%1%'",
                       path);
    mode_t mode = st.st_mode & ~S_IFMT;
    assert(S_ISLNK(st.st_mode) ||
           (st.st_uid == geteuid() && (mode == 0444 || mode == 0555) && st.st_mtime == mtime_store));
    return;
  }
#endif

  inodes_seen.insert(Inode(st.st_dev, st.st_ino));

  canonicalise_timestamp_and_permissions(path, st);

#ifndef _WIN32
  /* Change ownership to the current uid.  If it's a symlink, use
     lchown if available, otherwise don't bother.  Wrong ownership
     of a symlink doesn't matter, since the owning user can't change
     the symlink and can't delete it because the directory is not
     writable.  The only exception is top-level paths in the Nix
     store (since that directory is group-writable for the Nix build
     users group); we check for this case below. */
  if (st.st_uid != geteuid()) {
#  if HAVE_LCHOWN
    if (lchown(path.c_str(), geteuid(), getegid()) == -1)
#  else
    if (!S_ISLNK(st.st_mode) && chown(path.c_str(), geteuid(), getegid()) == -1)
#  endif
      throw sys_error_t("changing owner of '%1%' to %2%", path, geteuid());
  }
#endif

  if (S_ISDIR(st.st_mode)) {
    for (auto& i : directory_iterator_t{path}) {
      check_interrupt();
      canonicalise_path_meta_data_(i.path().string(),
#ifndef _WIN32
                                uidRange,
#endif
                                inodes_seen);
    }
  }
}

void canonicalise_path_meta_data(const Path& path,
#ifndef _WIN32
                              std::optional<std::pair<uid_t, uid_t>> uidRange,
#endif
                              InodesSeen& inodes_seen) {
  canonicalise_path_meta_data_(path,
#ifndef _WIN32
                            uidRange,
#endif
                            inodes_seen);

#ifndef _WIN32
  /* On platforms that don't have lchown(), the top-level path can't
     be a symlink, since we can't change its ownership. */
  auto st = lstat(path);

  if (st.st_uid != geteuid()) {
    assert(S_ISLNK(st.st_mode));
    throw Error("wrong ownership of top-level store path '%1%'", path);
  }
#endif
}

void canonicalise_path_meta_data(const Path& path
#ifndef _WIN32
                              ,
                              std::optional<std::pair<uid_t, uid_t>> uidRange
#endif
) {
  InodesSeen inodes_seen;
  canonicalise_path_meta_data_(path,
#ifndef _WIN32
                            uidRange,
#endif
                            inodes_seen);
}

} // namespace nix
