#include "nix/util/posix-source-accessor.h"

#include <fcntl.h>

#include <boost/unordered/concurrent_flat_map.hpp>

#include "nix/util/signals.h"
#include "nix/util/source-path.h"
#include "nix/util/sync.h"

namespace nix {

posix_source_accessor_t::posix_source_accessor_t(std::filesystem::path&& arg_root,
                                                 bool track_last_modified)
    : root(std::move(arg_root)), track_last_modified(track_last_modified) {
  assert(root.empty() || root.is_absolute());
  display_prefix = root.string();
}

posix_source_accessor_t::posix_source_accessor_t()
    : posix_source_accessor_t(std::filesystem::path{}) {}

source_path_t posix_source_accessor_t::create_at_root(const std::filesystem::path& path,
                                                      bool track_last_modified) {
  std::filesystem::path path2 = abs_path(path);
  return {
      make_ref<posix_source_accessor_t>(path2.root_path(), track_last_modified),
      canon_path_t{path2.relative_path().string()},
  };
}

std::filesystem::path posix_source_accessor_t::make_abs_path(const canon_path_t& path) {
  return root.empty()     ? (std::filesystem::path{path.abs()})
         : path.is_root() ? /* Don't append a slash for the root of the accessor, since
                              it can be a non-directory (e.g. in the case of `fetch_tree
                              { type = "file" }`). */
             root
                          : root / path.rel();
}

void posix_source_accessor_t::read_file(const canon_path_t& path, sink_t& sink,
                                        std::function<void(uint64_t)> size_callback) {
  assert_no_symlinks(path);

  auto ap = make_abs_path(path);

  auto_close_fd_t fd = to_descriptor(open(ap.string().c_str(), O_RDONLY
#ifndef _WIN32
                                                                   | O_NOFOLLOW | O_CLOEXEC
#endif
                                          ));
  if (!fd)
    throw sys_error_t("opening file '%1%'", ap.string());

  struct stat st;
  if (fstat(from_descriptor_read_only(fd.get()), &st) == -1)
    throw sys_error_t("statting file");

  size_callback(st.st_size);

  off_t left = st.st_size;

  std::array<unsigned char, 64 * 1024> buf;
  while (left) {
    check_interrupt();
    ssize_t rd = read(from_descriptor_read_only(fd.get()), buf.data(),
                      (size_t)std::min(left, (off_t)buf.size()));
    if (rd == -1) {
      if (errno != EINTR)
        throw sys_error_t("reading from file '%s'", show_path(path));
    } else if (rd == 0)
      throw sys_error_t("unexpected end-of-file reading '%s'", show_path(path));
    else {
      assert(rd <= left);
      sink({(char*)buf.data(), (size_t)rd});
      left -= rd;
    }
  }
}

bool posix_source_accessor_t::path_exists(const canon_path_t& path) {
  if (auto parent = path.parent())
    assert_no_symlinks(*parent);
  return nix::path_exists(make_abs_path(path).string());
}

using cache_t = boost::concurrent_flat_map<Path, std::optional<struct stat>>;
static cache_t cache;

std::optional<struct stat> posix_source_accessor_t::cached_lstat(const canon_path_t& path) {
  // Note: we convert std::filesystem::path to Path because the
  // former is not hashable on libc++.
  Path abs_path = make_abs_path(path).string();

  if (auto res = get_concurrent(cache, abs_path))
    return *res;

  auto st = nix::maybe_lstat(abs_path.c_str());

  if (cache.size() >= 16384)
    cache.clear();
  cache.emplace(std::move(abs_path), st);

  return st;
}

void posix_source_accessor_t::invalidate_cache(const canon_path_t& path) {
  cache.erase(make_abs_path(path).string());
}

std::optional<source_accessor_t::stat_t>
posix_source_accessor_t::maybe_lstat(const canon_path_t& path) {
  if (auto parent = path.parent())
    assert_no_symlinks(*parent);
  auto st = cached_lstat(path);
  if (!st)
    return std::nullopt;

  /* The contract is that track_last_modified implies that the caller uses the accessor
     from a single thread. Thus this is not a CAS loop. */
  if (track_last_modified)
    mtime = std::max(mtime, st->st_mtime);

  return stat_t{
      .type = S_ISREG(st->st_mode)   ? t_regular
              : S_ISDIR(st->st_mode) ? t_directory
              : S_ISLNK(st->st_mode) ? t_symlink
              : S_ISCHR(st->st_mode) ? t_char
              : S_ISBLK(st->st_mode) ? t_block
              :
#ifdef S_ISSOCK
              S_ISSOCK(st->st_mode) ? t_socket
              :
#endif
              S_ISFIFO(st->st_mode) ? t_fifo
                                    : t_unknown,
      .file_size = S_ISREG(st->st_mode) ? std::optional<uint64_t>(st->st_size) : std::nullopt,
      .is_executable = S_ISREG(st->st_mode) && st->st_mode & S_IXUSR,
  };
}

source_accessor_t::dir_entries_t posix_source_accessor_t::read_directory(const canon_path_t& path) {
  assert_no_symlinks(path);
  dir_entries_t res;
  for (auto& entry : directory_iterator_t{make_abs_path(path)}) {
    check_interrupt();
    auto type = [&]() -> std::optional<Type> {
      try {
        /* WARNING: We are specifically not calling symlink_status()
         * here, because that always translates to `stat` call and
         * doesn't make use of any caching. Instead, we have to
         * rely on the myriad of `is_*` functions, which actually do
         * the caching. If you are in doubt then take a look at the
         * libstdc++ implementation [1] and the standard proposal
         * about the caching variations of directory_entry [2].

         * [1]:
         https://github.com/gcc-mirror/gcc/blob/8ea555b7b4725dbc5d9286f729166cd54ce5b615/libstdc%2B%2B-v3/include/bits/fs_dir.h#L341-L348
         * [2]: https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2016/p0317r1.html
         */

        /* Check for symlink first, because other getters follow symlinks. */
        if (entry.is_symlink())
          return t_symlink;
        if (entry.is_regular_file())
          return t_regular;
        if (entry.is_directory())
          return t_directory;
        if (entry.is_character_file())
          return t_char;
        if (entry.is_block_file())
          return t_block;
        if (entry.is_fifo())
          return t_fifo;
        if (entry.is_socket())
          return t_socket;
        return t_unknown;
      } catch (std::filesystem::filesystem_error& e) {
        // We cannot always stat the child. (Ideally there is no
        // stat because the native directory entry has the type
        // already, but this isn't always the case.)
        if (e.code() == std::errc::permission_denied ||
            e.code() == std::errc::operation_not_permitted)
          return std::nullopt;
        else
          throw;
      }
    }();
    res.emplace(entry.path().filename().string(), type);
  }
  return res;
}

std::string posix_source_accessor_t::read_link(const canon_path_t& path) {
  if (auto parent = path.parent())
    assert_no_symlinks(*parent);
  return nix::read_link(make_abs_path(path).string());
}

std::optional<std::filesystem::path>
posix_source_accessor_t::get_physical_path(const canon_path_t& path) {
  return make_abs_path(path);
}

void posix_source_accessor_t::assert_no_symlinks(canon_path_t path) {
  while (!path.is_root()) {
    auto st = cached_lstat(path);
    if (st && S_ISLNK(st->st_mode))
      throw Error("path '%s' is a symlink", show_path(path));
    path.pop();
  }
}

ref<source_accessor_t> get_fs_source_accessor() {
  static auto root_fs = make_ref<posix_source_accessor_t>();
  return root_fs;
}

ref<source_accessor_t> make_fs_source_accessor(std::filesystem::path root) {
  return make_ref<posix_source_accessor_t>(std::move(root));
}
} // namespace nix
