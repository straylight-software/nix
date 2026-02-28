#include <cstdlib>
#include <cstring>
#include <regex>

#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "nix/store/globals.h"
#include "nix/store/local-store.h"
#include "nix/store/posix-fs-canonicalise.h"
#include "nix/util/posix-source-accessor.h"
#include "nix/util/signals.h"
#include "store-config-private.h"

namespace nix {

static void make_writable(const Path& path) {
  auto st = lstat(path);
  if (chmod(path.c_str(), st.st_mode | S_IWUSR) == -1)
    throw sys_error_t("changing writability of '%1%'", path);
}

struct make_read_only_t {
  Path path;

  make_read_only_t(const path_view_t path) : path(path) {}

  ~make_read_only_t() {
    try {
      /* This will make the path read-only. */
      if (path != "")
        canonicalise_timestamp_and_permissions(path);
    } catch (...) {
      ignore_exception_in_destructor();
    }
  }
};

LocalStore::InodeHash LocalStore::loadInodeHash() {
  debug("loading hash inodes in memory");
  InodeHash inodeHash;

  auto_close_dir_t dir(opendir(linksDir.c_str()));
  if (!dir)
    throw sys_error_t("opening directory '%1%'", linksDir);

  struct dirent* dirent;
  while (errno = 0, dirent = readdir(dir.get())) { /* sic */
    check_interrupt();
    // We don't care if we hit non-hash files, anything goes
    inodeHash.insert(dirent->d_ino);
  }
  if (errno)
    throw sys_error_t("reading directory '%1%'", linksDir);

  printMsg(lvl_talkative, "loaded %1% hash inodes", inodeHash.size());

  return inodeHash;
}

strings_t LocalStore::readDirectoryIgnoringInodes(const Path& path, const InodeHash& inodeHash) {
  strings_t names;

  auto_close_dir_t dir(opendir(path.c_str()));
  if (!dir)
    throw sys_error_t("opening directory '%1%'", path);

  struct dirent* dirent;
  while (errno = 0, dirent = readdir(dir.get())) { /* sic */
    check_interrupt();

    if (inodeHash.count(dirent->d_ino)) {
      debug("'%1%' is already linked", dirent->d_name);
      continue;
    }

    std::string name = dirent->d_name;
    if (name == "." || name == "..")
      continue;
    names.push_back(name);
  }
  if (errno)
    throw sys_error_t("reading directory '%1%'", path);

  return names;
}

void LocalStore::optimisePath_(activity_t* act, OptimiseStats& stats, const Path& path,
                               InodeHash& inodeHash, RepairFlag repair) {
  check_interrupt();

  auto st = lstat(path);

#ifdef __APPLE__
  /* HFS/macOS has some undocumented security feature disabling hardlinking for
     special files within .app dirs. Known affected paths include
     *.app/Contents/{PkgInfo,Resources/\*.lproj,_CodeSignature} and .DS_Store.
     See https://github.com/NixOS/nix/issues/1443 and
     https://github.com/NixOS/nix/pull/2230 for more discussion. */

  if (std::regex_search(path, std::regex("\\.app/Contents/.+$"))) {
    debug("'%1%' is not allowed to be linked in macOS", path);
    return;
  }
#endif

  if (S_ISDIR(st.st_mode)) {
    strings_t names = readDirectoryIgnoringInodes(path, inodeHash);
    for (auto& i : names)
      optimisePath_(act, stats, path + "/" + i, inodeHash, repair);
    return;
  }

  /* We can hard link regular files and maybe symlinks. */
  if (!S_ISREG(st.st_mode)
#if CAN_LINK_SYMLINK
      && !S_ISLNK(st.st_mode)
#endif
  )
    return;

  /* Sometimes SNAFUs can cause files in the Nix store to be
     modified, in particular when running programs as root under
     NixOS (example: $fontconfig/var/cache being modified).  Skip
     those files.  FIXME: check the modification time. */
  if (S_ISREG(st.st_mode) && (st.st_mode & S_IWUSR)) {
    warn("skipping suspicious writable file '%1%'", path);
    return;
  }

  /* This can still happen on top-level files. */
  if (st.st_nlink > 1 && inodeHash.count(st.st_ino)) {
    debug("'%s' is already linked, with %d other file(s)", path, st.st_nlink - 2);
    return;
  }

  /* Hash the file.  Note that hash_path() returns the hash over the
     NAR serialisation, which includes the execute bit on the file.
     Thus, executable and non-executable files with the same
     contents *won't* be linked (which is good because otherwise the
     permissions would be screwed up).

     Also note that if `path' is a symlink, then we're hashing the
     contents of the symlink (i.e. the result of readlink()), not
     the contents of the target (which may not even exist).

     To prevent a race condition where we hash a file while another
     process is still writing to it (see NixOS/nix#14599), we verify
     that the file's metadata hasn't changed between before and after
     hashing. If the file changed during hashing, we skip it - it will
     be optimized on the next run when the write is complete. */
  Hash hash = ({
    hash_path({make_ref<posix_source_accessor_t>(), canon_path_t(path)},
              file_serialisation_method_t::nix_archive, hash_algorithm_t::SHA256)
        .hash;
  });

  /* Re-stat the file to detect if it changed while we were hashing.
     This mitigates the race condition where another process is writing
     to this file concurrently. We check inode, size, and mtime. */
  auto stAfterHash = lstat(path);
  if (st.st_ino != stAfterHash.st_ino || st.st_size != stAfterHash.st_size ||
      st.st_mtime != stAfterHash.st_mtime) {
    debug("'%1%' changed while hashing, skipping optimization", path);
    return;
  }

  debug("'%1%' has hash '%2%'", path, hash.to_string(hash_format_t::nix32, true));

  /* Check if this is a known hash. */
  std::filesystem::path linkPath =
      std::filesystem::path{linksDir} / hash.to_string(hash_format_t::nix32, false);

  /* Maybe delete the link, if it has been corrupted. */
  if (std::filesystem::exists(std::filesystem::symlink_status(linkPath))) {
    auto stLink = lstat(linkPath.string());
    if (st.st_size != stLink.st_size ||
        (repair && hash != ({
                     hash_path(make_fs_source_accessor(linkPath),
                               file_serialisation_method_t::nix_archive, hash_algorithm_t::SHA256)
                         .hash;
                   }))) {
      // XXX: Consider overwriting linkPath with our valid version.
      warn("removing corrupted link %s", linkPath);
      warn("There may be more corrupted paths."
           "\nYou should run `nix-store --verify --check-contents --repair` to fix them all");
      std::filesystem::remove(linkPath);
    }
  }

  if (!std::filesystem::exists(std::filesystem::symlink_status(linkPath))) {
    /* Nope, create a hard link in the links directory. */
    try {
      std::filesystem::create_hard_link(path, linkPath);
      inodeHash.insert(st.st_ino);
    } catch (std::filesystem::filesystem_error& e) {
      if (e.code() == std::errc::file_exists) {
        /* Fall through if another process created ‘linkPath’ before
           we did. */
      }

      else if (e.code() == std::errc::no_space_on_device) {
        /* On ext4, that probably means the directory index is
           full.  When that happens, it's fine to ignore it: we
           just effectively disable deduplication of this
           file.  */
        printInfo("cannot link %s to '%s': %s", linkPath, path, strerror(errno));
        return;
      }

      else
        throw;
    }
  }

  /* yes!  We've seen a file with the same contents.  Replace the
     current file with a hard link to that file. */
  auto stLink = lstat(linkPath.string());

  if (st.st_ino == stLink.st_ino) {
    debug("'%1%' is already linked to %2%", path, linkPath);
    return;
  }

  printMsg(lvl_talkative, "linking '%1%' to %2%", path, linkPath);

  /* Make the containing directory writable, but only if it's not
     the store itself (we don't want or need to mess with its
     permissions). */
  const Path dirOfPath(dir_of(path));
  bool mustToggle = dirOfPath != config->real_store_dir.get();
  if (mustToggle)
    make_writable(dirOfPath);

  /* When we're done, make the directory read-only again and reset
     its timestamp back to 0. */
  make_read_only_t makeReadOnly(mustToggle ? dirOfPath : "");

  std::filesystem::path tempLink = make_temp_path(config->real_store_dir.get(), ".tmp-link");

  /* Handle race condition with GC: GC may delete a link in .links/
     just as we're about to hard-link to it. GC deletes links with
     st_nlink == 1, but between GC's stat() and unlink(), we might
     try to create a hard link. If GC wins the race and deletes the
     link first, we get ENOENT. In that case, recreate the link from
     our source file `path` and retry. */
  for (int retries = 0;; ++retries) {
    try {
      std::filesystem::create_hard_link(linkPath, tempLink);
      inodeHash.insert(st.st_ino);
      break;
    } catch (std::filesystem::filesystem_error& e) {
      if (e.code() == std::errc::too_many_links) {
        /* Too many links to the same file (>= 32000 on most file
           systems).  This is likely to happen with empty files.
           Just shrug and ignore. */
        if (st.st_size)
          printInfo("%1% has maximum number of links", linkPath);
        return;
      }

      if (e.code() == std::errc::no_such_file_or_directory && retries < 3) {
        /* The link in .links/ was deleted by GC between our check
           for its existence and the hard link attempt. Recreate it
           from the source file. */
        debug("link '%s' was deleted by GC, recreating from '%s'", linkPath, path);
        try {
          std::filesystem::create_hard_link(path, linkPath);
          /* Successfully recreated; retry the link to tempLink. */
          continue;
        } catch (std::filesystem::filesystem_error& e2) {
          if (e2.code() == std::errc::file_exists) {
            /* Another process recreated it; retry. */
            continue;
          }
          throw;
        }
      }

      throw;
    }
  }

  /* Final safety check before replacing: verify that the link target
     still has the expected content. This catches the case where another
     optimization process incorrectly linked a file that was being written
     to (see NixOS/nix#14599). If the link file has wrong content, we must
     not link to it - instead we remove the corrupted link and abort. */
  {
    auto stLinkFinal = lstat(linkPath.string());
    if (st.st_size != stLinkFinal.st_size) {
      warn("link file '%s' has unexpected size (expected %d, got %d), removing corrupted link",
           linkPath.string(), st.st_size, stLinkFinal.st_size);
      std::error_code ec;
      std::filesystem::remove(tempLink, ec);
      std::filesystem::remove(linkPath, ec);
      return;
    }
  }

  /* Atomically replace the old file with the new hard link. */
  try {
    std::filesystem::rename(tempLink, path);
  } catch (std::filesystem::filesystem_error& e) {
    {
      std::error_code ec;
      remove(tempLink, ec); /* Clean up after ourselves. */
      if (ec)
        printError("unable to unlink %1%: %2%", tempLink, ec.message());
    }
    if (e.code() == std::errc::too_many_links) {
      /* Some filesystems generate too many links on the rename,
         rather than on the original link.  (Probably it
         temporarily increases the st_nlink field before
         decreasing it again.) */
      debug("%s has reached maximum number of links", linkPath);
      return;
    }
    throw;
  }

  stats.files_linked++;
  stats.bytes_freed += st.st_size;

  if (act)
    act->result(res_file_linked, st.st_size
#ifndef _WIN32
                ,
                st.st_blocks
#endif
    );
}

void LocalStore::optimiseStore(OptimiseStats& stats) {
  activity_t act(*logger, act_optimise_store);

  auto paths = query_all_valid_paths();
  InodeHash inodeHash = loadInodeHash();

  act.progress(0, paths.size());

  uint64_t done = 0;

  for (auto& i : paths) {
    addTempRoot(i);
    if (!isValidPath(i))
      continue; /* path was GC'ed, probably */
    {
      activity_t act(*logger, lvl_talkative, act_unknown,
                     fmt("optimising path '%s'", printStorePath(i)));
      optimisePath_(&act, stats, config->real_store_dir + "/" + std::string(i.to_string()),
                    inodeHash, NoRepair);
    }
    done++;
    act.progress(done, paths.size());
  }
}

void LocalStore::optimiseStore() {
  OptimiseStats stats;

  optimiseStore(stats);

  printInfo("%s freed by hard-linking %d files", render_size(stats.bytes_freed),
            stats.files_linked);
}

void LocalStore::optimisePath(const Path& path, RepairFlag repair) {
  OptimiseStats stats;
  InodeHash inodeHash;

  if (settings.autoOptimiseStore)
    optimisePath_(nullptr, stats, path, inodeHash, repair);
}

} // namespace nix
