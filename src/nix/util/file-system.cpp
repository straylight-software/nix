#include "nix/util/file-system.h"

#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <random>

#include <fcntl.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include <boost/iostreams/device/mapped_file.hpp>

#include "nix/util/environment-variables.h"
#include "nix/util/file-path-impl.h"
#include "nix/util/file-path.h"
#include "nix/util/finally.h"
#include "nix/util/serialise.h"
#include "nix/util/signals.h"
#include "nix/util/util.h"

#ifdef __FreeBSD__
#  include <sys/mount.h>
#  include <sys/param.h>
#endif

#ifdef _WIN32
#  include <io.h>
#endif

namespace nix {

directory_iterator_t::directory_iterator_t(const std::filesystem::path& p) {
  try {
    // **Attempt to create the underlying directory_iterator**
    it_ = std::filesystem::directory_iterator(p);
  } catch (const std::filesystem::filesystem_error& e) {
    // **Catch filesystem_error and throw SysError**
    // Adapt the error message as needed for SysError
    throw sys_error_t("cannot read directory %s", p);
  }
}

directory_iterator_t& directory_iterator_t::operator++() {
  // **Attempt to increment the underlying iterator**
  std::error_code ec;
  it_.increment(ec);
  if (ec) {
    // Try to get path info if possible, might fail if iterator is bad
    try {
      if (it_ != std::filesystem::directory_iterator{}) {
        throw sys_error_t("cannot read directory past %s: %s", it_->path(), ec.message());
      }
    } catch (...) {
      throw sys_error_t("cannot read directory");
    }
  }
  return *this;
}

bool is_absolute(path_view_t path) {
  return std::filesystem::path{path}.is_absolute();
}

Path abs_path(path_view_t path, std::optional<path_view_t> dir, bool resolve_symlinks) {
  std::string scratch;

  if (!is_absolute(path)) {
    // In this case we need to call `canonPath` on a newly-created
    // string. We set `scratch` to that string first, and then set
    // `path` to `scratch`. This ensures the newly-created string
    // lives long enough for the call to `canonPath`, and allows us
    // to just accept a `std::string_view`.
    if (!dir) {
#ifdef __GNU__
      /* GNU (aka. GNU/Hurd) doesn't have any limitation on path
         lengths and doesn't define `PATH_MAX'.  */
      char* buf = getcwd(NULL, 0);
      if (buf == NULL)
#else
      char buf[PATH_MAX];
      if (!getcwd(buf, sizeof(buf))) {
#endif
        throw sys_error_t("cannot get cwd");
}
      scratch = concat_strings(buf, "/", path);
#ifdef __GNU__
      free(buf);
#endif
    } else {
      scratch = concat_strings(*dir, "/", path);
}
    path = scratch;
  }
  return canon_path(path, resolve_symlinks);
}

std::filesystem::path abs_path(const std::filesystem::path& path, const std::filesystem::path* dir_,
                              bool resolve_symlinks) {
  std::optional<std::string> dir = dir_ ? std::optional<std::string>{dir_->string()} : std::nullopt;
  return abs_path(path_view_t{path.string()}, dir.transform([](auto& p) { return path_view_t(p); }),
                 resolve_symlinks);
}

Path canon_path(path_view_t path, bool resolve_symlinks) {
  assert(path != "");

  if (!is_absolute(path)) {
    throw Error("not an absolute path: '%1%'", path);
}

  // For Windows
  auto root_name = std::filesystem::path{path}.root_name();

  /* This just exists because we cannot set the target of `remaining`
     (the callback parameter) directly to a newly-constructed string,
     since it is `std::string_view`. */
  std::string temp;

  /* Count the number of times we follow a symlink and stop at some
     arbitrary (but high) limit to prevent infinite loops. */
  unsigned int follow_count = 0, max_follow = 1024;

  auto ret = canon_path_inner<os_path_trait_t<char>>(
      path, [&follow_count, &temp, max_follow, resolve_symlinks](std::string& result,
                                                              std::string_view& remaining) {
        if (resolve_symlinks && std::filesystem::is_symlink(result)) {
          if (++follow_count >= max_follow) {
            throw Error("infinite symlink recursion in path '%1%'", remaining);
}
          remaining = (temp = concat_strings(read_link(result), remaining));
          if (is_absolute(remaining)) {
            /* restart for symlinks pointing to absolute path */
            result.clear();
          } else {
            result = dir_of(result);
            if (result == "/") {
              /* we don’t want trailing slashes here, which `dir_of`
                 only produces if `result = /` */
              result.clear();
            }
          }
        }
      });

  if (!root_name.empty()) {
    ret = root_name.string() + std::move(ret);
}
  return ret;
}

Path dir_of(const path_view_t path) {
  Path::size_type pos = os_path_trait_t<char>::rfind_path_sep(path);
  if (pos == path.npos) {
    return ".";
}
  return std::filesystem::path{path}.parent_path().string();
}

std::string_view base_name_of(std::string_view path) {
  if (path.empty()) {
    return "";
}

  auto last = path.size() - 1;
  while (last > 0 && os_path_trait_t<char>::is_path_sep(path[last])) {
    last -= 1;
}

  auto pos = os_path_trait_t<char>::rfind_path_sep(path, last);
  if (pos == path.npos) {
    pos = 0;
  } else {
    pos += 1;
}

  return path.substr(pos, last - pos + 1);
}

bool is_in_dir(const std::filesystem::path& path, const std::filesystem::path& dir) {
  /* Note that while the standard doesn't guarantee this, the
    `lexically_*` functions should do no IO and not throw. */
  auto rel = path.lexically_relative(dir);
  /* Method from
     https://stackoverflow.com/questions/62503197/check-if-path-contains-another-in-c++ */
  return !rel.empty() && rel.native()[0] != OS_STR('.');
}

bool is_dir_or_in_dir(const std::filesystem::path& path, const std::filesystem::path& dir) {
  return path == dir || is_in_dir(path, dir);
}

struct stat stat(const Path& path) {
  struct stat st;
  if (stat(path.c_str(), &st)) {
    throw sys_error_t("getting status of '%1%'", path);
}
  return st;
}

#ifdef _WIN32
#  define STAT stat
#else
#  define STAT lstat
#endif

struct stat lstat(const Path& path) {
  struct stat st;
  if (STAT(path.c_str(), &st)) {
    throw sys_error_t("getting status of '%1%'", path);
}
  return st;
}

std::optional<struct stat> maybe_lstat(const Path& path) {
  std::optional<struct stat> st{std::in_place};
  if (STAT(path.c_str(), &*st)) {
    if (errno == ENOENT || errno == ENOTDIR) {
      st.reset();
    } else {
      throw sys_error_t("getting status of '%s'", path);
}
  }
  return st;
}

bool path_exists(const std::filesystem::path& path) {
  return maybe_lstat(path.string()).has_value();
}

bool path_accessible(const std::filesystem::path& path) {
  try {
    return path_exists(path.string());
  } catch (sys_error_t& e) {
    // swallow EPERM
    if (e.err_no == EPERM) {
      return false;
}
    throw;
  }
}

std::filesystem::path read_link(const std::filesystem::path& path) {
  check_interrupt();
  return std::filesystem::read_symlink(path);
}

Path read_link(const Path& path) {
  return read_link(std::filesystem::path{path}).string();
}

std::string read_file(const Path& path) {
  auto_close_fd_t fd = to_descriptor(open(path.c_str(), O_RDONLY
#ifdef O_CLOEXEC
                                                       | O_CLOEXEC
#endif
                                     ));
  if (!fd) {
    throw sys_error_t("opening file '%1%'", path);
}
  return read_file(fd.get());
}

std::string read_file(const std::filesystem::path& path) {
  return read_file(os_string_to_string(path_view_ng_t{path}));
}

void read_file(const Path& path, Sink& sink, bool memory_map) {
  // Memory-map the file for faster processing where possible.
  if (memory_map) {
    try {
      boost::iostreams::mapped_file_source mmap(path);
      if (mmap.is_open()) {
        sink({mmap.data(), mmap.size()});
        return;
      }
    } catch (const boost::exception& e) {
    }
    debug("memory-mapping failed for path: %s", path);
  }

  // Stream the file instead if memory-mapping fails or is disabled.
  auto_close_fd_t fd = to_descriptor(open(path.c_str(), O_RDONLY
#ifdef O_CLOEXEC
                                                       | O_CLOEXEC
#endif
                                     ));
  if (!fd) {
    throw sys_error_t("opening file '%s'", path);
}
  drain_fd(fd.get(), sink);
}

void write_file(const Path& path, std::string_view s, mode_t mode, fs_sync_t sync) {
  auto_close_fd_t fd = to_descriptor(open(path.c_str(),
                                     O_WRONLY | O_TRUNC | O_CREAT
#ifdef O_CLOEXEC
                                         | O_CLOEXEC
#endif
                                     ,
                                     mode));
  if (!fd) {
    throw sys_error_t("opening file '%1%'", path);
}

  write_file(fd, path, s, mode, sync);

  /* Close explicitly to propagate the exceptions. */
  fd.close();
}

void write_file(auto_close_fd_t& fd, const Path& orig_path, std::string_view s, mode_t mode,
               fs_sync_t sync) {
  assert(fd);
  try {
    write_full(fd.get(), s);

    if (sync == fs_sync_t::yes) {
      fd.fsync();
}

  } catch (Error& e) {
    e.add_trace({}, "writing file '%1%'", orig_path);
    throw;
  }
}

void write_file(const Path& path, Source& source, mode_t mode, fs_sync_t sync) {
  auto_close_fd_t fd = to_descriptor(open(path.c_str(),
                                     O_WRONLY | O_TRUNC | O_CREAT
#ifdef O_CLOEXEC
                                         | O_CLOEXEC
#endif
                                     ,
                                     mode));
  if (!fd) {
    throw sys_error_t("opening file '%1%'", path);
}

  std::array<char, 64 * 1024> buf;

  try {
    while (true) {
      try {
        auto n = source.read(buf.data(), buf.size());
        write_full(fd.get(), {buf.data(), n});
      } catch (EndOfFile&) {
        break;
      }
    }
  } catch (Error& e) {
    e.add_trace({}, "writing file '%1%'", path);
    throw;
  }
  if (sync == fs_sync_t::yes) {
    fd.fsync();
}
  // Explicitly close to make sure exceptions are propagated.
  fd.close();
  if (sync == fs_sync_t::yes) {
    sync_parent(path);
}
}

void sync_parent(const Path& path) {
  auto_close_fd_t fd = to_descriptor(open(dir_of(path).c_str(), O_RDONLY, 0));
  if (!fd) {
    throw sys_error_t("opening file '%1%'", path);
}
  fd.fsync();
}

#ifdef __FreeBSD__
#  define MOUNTEDPATHS_PARAM , std::set<Path>& mountedPaths
#  define MOUNTEDPATHS_ARG , mountedPaths
#else
#  define MOUNTEDPATHS_PARAM
#  define MOUNTEDPATHS_ARG
#endif

void recursive_sync(const Path& path) {
  /* If it's a file or symlink, just fsync and return. */
  auto st = lstat(path);
  if (S_ISREG(st.st_mode)) {
    auto_close_fd_t fd = to_descriptor(open(path.c_str(), O_RDONLY, 0));
    if (!fd) {
      throw sys_error_t("opening file '%1%'", path);
}
    fd.fsync();
    return;
  } else if (S_ISLNK(st.st_mode)) {
    return;
}

  /* Otherwise, perform a depth-first traversal of the directory and
     fsync all the files. */
  std::deque<std::filesystem::path> dirs_to_enumerate;
  dirs_to_enumerate.push_back(path);
  std::vector<std::filesystem::path> dirs_to_fsync;
  while (!dirs_to_enumerate.empty()) {
    auto current_dir = dirs_to_enumerate.back();
    dirs_to_enumerate.pop_back();
    for (auto& entry : directory_iterator_t(current_dir)) {
      auto st = entry.symlink_status();
      if (std::filesystem::is_directory(st)) {
        dirs_to_enumerate.emplace_back(entry.path());
      } else if (std::filesystem::is_regular_file(st)) {
        auto_close_fd_t fd = to_descriptor(open(entry.path().string().c_str(), O_RDONLY, 0));
        if (!fd) {
          throw sys_error_t("opening file '%1%'", entry.path());
}
        fd.fsync();
      }
    }
    dirs_to_fsync.emplace_back(std::move(current_dir));
  }

  /* Fsync all the directories. */
  for (auto dir = dirs_to_fsync.rbegin(); dir != dirs_to_fsync.rend(); ++dir) {
    auto_close_fd_t fd = to_descriptor(open(dir->string().c_str(), O_RDONLY, 0));
    if (!fd) {
      throw sys_error_t("opening directory '%1%'", *dir);
}
    fd.fsync();
  }
}

static void delete_path_(descriptor_t parentfd, const std::filesystem::path& path,
                        uint64_t& bytes_freed, std::exception_ptr& ex MOUNTEDPATHS_PARAM) {
#ifndef _WIN32
  check_interrupt();

#  ifdef __FreeBSD__
  // In case of emergency (unmount fails for some reason) not recurse into mountpoints.
  // This prevents us from tearing up the nullfs-mounted nix store.
  if (mountedPaths.find(path) != mountedPaths.end()) {
    return;
  }
#  endif

  std::string name(path.filename());
  assert(name != "." && name != ".." && !name.empty());

  struct stat st;
  if (fstatat(parentfd, name.c_str(), &st, AT_SYMLINK_NOFOLLOW) == -1) {
    if (errno == ENOENT) {
      return;
}
    throw sys_error_t("getting status of %1%", path);
  }

  if (!S_ISDIR(st.st_mode)) {
    /* We are about to delete a file. Will it likely free space? */

    switch (st.st_nlink) {
      /* yes: last link. */
      case 1:
        bytes_freed += st.st_size;
        break;
      /* Maybe: yes, if 'auto-optimise-store' or manual optimisation
         was performed. Instead of checking for real let's assume
         it's an optimised file and space will be freed.

         In worst case we will double count on freed space for files
         with exactly two hardlinks for unoptimised packages.
       */
      case 2:
        bytes_freed += st.st_size;
        break;
      /* No: 3+ links. */
      default:
        break;
    }
  }

  if (S_ISDIR(st.st_mode)) {
    /* Make the directory accessible. */
    const auto PERM_MASK = S_IRUSR | S_IWUSR | S_IXUSR;
    if ((st.st_mode & PERM_MASK) != PERM_MASK) {
      if (fchmodat(parentfd, name.c_str(), st.st_mode | PERM_MASK, 0) == -1) {
        throw sys_error_t("chmod %1%", path);
}
    }

    int fd = openat(parentfd, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (fd == -1) {
      throw sys_error_t("opening directory %1%", path);
}
    auto_close_dir_t dir(fdopendir(fd));
    if (!dir) {
      throw sys_error_t("opening directory %1%", path);
}

    struct dirent* dirent;
    while (errno = 0, dirent = readdir(dir.get())) { /* sic */
      check_interrupt();
      std::string child_name = dirent->d_name;
      if (child_name == "." || child_name == "..") {
        continue;
}
      delete_path_(dirfd(dir.get()), path / child_name, bytes_freed, ex MOUNTEDPATHS_ARG);
    }
    if (errno) {
      throw sys_error_t("reading directory %1%", path);
}
  }

  int flags = S_ISDIR(st.st_mode) ? AT_REMOVEDIR : 0;
  if (unlinkat(parentfd, name.c_str(), flags) == -1) {
    if (errno == ENOENT) {
      return;
}
    try {
      throw sys_error_t("cannot unlink %1%", path);
    } catch (...) {
      if (!ex) {
        ex = std::current_exception();
      } else {
        ignore_exception_except_interrupt();
}
    }
  }
#else
  // TODO implement
  throw UnimplementedError("_deletePath");
#endif
}

static void delete_path_(const std::filesystem::path& path,
                        uint64_t& bytes_freed MOUNTEDPATHS_PARAM) {
  assert(path.is_absolute());
  assert(path.parent_path() != path);

  auto_close_fd_t dirfd = to_descriptor(open(path.parent_path().string().c_str(), O_RDONLY));
  if (!dirfd) {
    if (errno == ENOENT) {
      return;
}
    throw sys_error_t("opening directory %s", path.parent_path());
  }

  std::exception_ptr ex;

  delete_path_(dirfd.get(), path, bytes_freed, ex MOUNTEDPATHS_ARG);

  if (ex) {
    std::rethrow_exception(ex);
}
}

void delete_path(const std::filesystem::path& path) {
  uint64_t dummy;
  delete_path(path, dummy);
}

void create_dir(const Path& path, mode_t mode) {
  if (mkdir(path.c_str()
#ifndef _WIN32
                ,
            mode
#endif
            ) == -1) {
    throw sys_error_t("creating directory '%1%'", path);
}
}

void create_dirs(const std::filesystem::path& path) {
  try {
    std::filesystem::create_directories(path);
  } catch (std::filesystem::filesystem_error& e) {
    throw sys_error_t("creating directory '%1%'", path.string());
  }
}

void delete_path(const std::filesystem::path& path, uint64_t& bytes_freed) {
  // Activity act(*logger, lvlDebug, "recursively deleting path '%1%'", path);
#ifdef __FreeBSD__
  std::set<Path> mountedPaths;
  struct statfs* mntbuf;
  int count;
  if ((count = getmntinfo(&mntbuf, MNT_WAIT)) < 0) {
    throw sys_error_t("getmntinfo");
  }

  for (int i = 0; i < count; i++) {
    mountedPaths.emplace(mntbuf[i].f_mntonname);
  }
#endif
  bytes_freed = 0;
  delete_path_(path, bytes_freed MOUNTEDPATHS_ARG);
}

//////////////////////////////////////////////////////////////////////

auto_delete_t::auto_delete_t() : del{false} {}

auto_delete_t::auto_delete_t(const std::filesystem::path& p, bool recursive) : _path(p) {
  del = true;
  this->recursive = recursive;
}

auto_delete_t::~auto_delete_t() {
  try {
    if (del) {
      if (recursive) {
        delete_path(_path);
      } else {
        std::filesystem::remove(_path);
      }
    }
  } catch (...) {
    ignore_exception_in_destructor();
  }
}

void auto_delete_t::cancel() {
  del = false;
}

void auto_delete_t::reset(const std::filesystem::path& p, bool recursive) {
  _path = p;
  this->recursive = recursive;
  del = true;
}

//////////////////////////////////////////////////////////////////////

#ifdef __FreeBSD__
AutoUnmount::AutoUnmount() : del{false} {}

AutoUnmount::AutoUnmount(Path& p) : path(p), del(true) {}

AutoUnmount::~AutoUnmount() {
  try {
    if (del) {
      if (unmount(path.c_str(), 0) < 0) {
        throw sys_error_t("Failed to unmount path %1%", path);
      }
    }
  } catch (...) {
    ignore_exception_in_destructor();
  }
}

void AutoUnmount::cancel() {
  del = false;
}
#endif

//////////////////////////////////////////////////////////////////////

std::filesystem::path default_temp_dir() {
  return get_env_non_empty("TMPDIR").value_or("/tmp");
}

std::filesystem::path create_temp_dir(const std::filesystem::path& tmp_root, const std::string& prefix,
                                    mode_t mode) {
  while (1) {
    check_interrupt();
    std::filesystem::path tmp_dir = make_temp_path(tmp_root, prefix);
    if (mkdir(tmp_dir.string().c_str()
#ifndef _WIN32 // TODO abstract mkdir perms for Windows
                  ,
              mode
#endif
              ) == 0) {
#ifdef __FreeBSD__
      /* Explicitly set the group of the directory.  This is to
         work around around problems caused by BSD's group
         ownership semantics (directories inherit the group of
         the parent).  For instance, the group of /tmp on
         FreeBSD is "wheel", so all directories created in /tmp
         will be owned by "wheel"; but if the user is not in
         "wheel", then "tar" will fail to unpack archives that
         have the setgid bit set on directories. */
      if (chown(tmp_dir.c_str(), (uid_t)-1, getegid()) != 0)
        throw sys_error_t("setting group of directory '%1%'", tmp_dir);
#endif
      return tmp_dir;
    }
    if (errno != EEXIST) {
      throw sys_error_t("creating directory '%1%'", tmp_dir);
}
  }
}

auto_close_fd_t create_anonymous_temp_file() {
  auto_close_fd_t fd;
#ifdef O_TMPFILE
  static std::atomic_flag tmpfile_unsupported{};
  if (!tmpfile_unsupported.test()) /* Try with O_TMPFILE first. */ {
    /* use O_EXCL, because the file is never supposed to be linked into filesystem. */
    fd = ::open(default_temp_dir().c_str(), O_TMPFILE | O_CLOEXEC | O_RDWR | O_EXCL,
                S_IWUSR | S_IRUSR);
    if (!fd) {
      /* Not supported by the filesystem or the kernel. */
      if (errno == EOPNOTSUPP || errno == EISDIR) {
        tmpfile_unsupported.test_and_set(); /* Set flag and fall through to create_temp_file. */
      } else {
        throw sys_error_t("creating anonymous temporary file");
}
    } else {
      return fd; /* Successfully created. */
    }
  }
#endif
  auto [fd2, path] = create_temp_file("nix-anonymous");
  if (!fd2) {
    throw sys_error_t("creating temporary file '%s'", path);
}
  fd = std::move(fd2);
#ifndef _WIN32
  unlink(require_c_string(path)); /* We only care about the file descriptor. */
#endif
  return fd;
}

std::pair<auto_close_fd_t, Path> create_temp_file(const Path& prefix) {
  Path tmpl(default_temp_dir().string() + "/" + prefix + ".XXXXXX");
  // Strictly speaking, this is UB, but who cares...
  // FIXME: use O_TMPFILE.
  // FIXME: Windows should use FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE
  auto_close_fd_t fd = to_descriptor(mkstemp((char*)tmpl.c_str()));
  if (!fd) {
    throw sys_error_t("creating temporary file '%s'", tmpl);
}
#ifndef _WIN32
  unix::close_on_exec(fd.get());
#endif
  return {std::move(fd), tmpl};
}

std::filesystem::path make_temp_path(const std::filesystem::path& root, const std::string& suffix) {
  // start the counter at a random value to minimize issues with preexisting temp paths
  static std::atomic<uint32_t> counter(std::random_device{}());
  auto tmp_root = canon_path(root.empty() ? default_temp_dir().string() : root.string(), true);
  return fmt("%1%/%2%-%3%-%4%", tmp_root, suffix, getpid(),
             counter.fetch_add(1, std::memory_order_relaxed));
}

void create_symlink(const Path& target, const Path& link) {
  std::error_code ec;
  std::filesystem::create_symlink(target, link, ec);
  if (ec) {
    throw sys_error_t(ec.value(), "creating symlink '%1%' -> '%2%'", link, target);
}
}

void replace_symlink(const std::filesystem::path& target, const std::filesystem::path& link) {
  for (unsigned int n = 0; true; n++) {
    auto tmp =
        link.parent_path() / std::filesystem::path{fmt(".%d_%s", n, link.filename().string())};
    tmp = tmp.lexically_normal();

    try {
      std::filesystem::create_symlink(target, tmp);
    } catch (std::filesystem::filesystem_error& e) {
      if (e.code() == std::errc::file_exists) {
        continue;
}
      throw sys_error_t("creating symlink %1% -> %2%", tmp, target);
    }

    try {
      std::filesystem::rename(tmp, link);
    } catch (std::filesystem::filesystem_error& e) {
      if (e.code() == std::errc::file_exists) {
        continue;
}
      throw sys_error_t("renaming %1% to %2%", tmp, link);
    }

    break;
  }
}

void set_write_time(const std::filesystem::path& path, const struct stat& st) {
  set_write_time(path, st.st_atime, st.st_mtime, S_ISLNK(st.st_mode));
}

void copy_file(const std::filesystem::path& from, const std::filesystem::path& to, bool and_delete) {
  auto from_status = std::filesystem::symlink_status(from);

  // Mark the directory as writable so that we can delete its children
  if (and_delete && std::filesystem::is_directory(from_status)) {
    std::filesystem::permissions(from, std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::add |
                                     std::filesystem::perm_options::nofollow);
  }

  if (std::filesystem::is_symlink(from_status) || std::filesystem::is_regular_file(from_status)) {
    std::filesystem::copy(from, to,
                          std::filesystem::copy_options::copy_symlinks |
                              std::filesystem::copy_options::overwrite_existing);
  } else if (std::filesystem::is_directory(from_status)) {
    std::filesystem::create_directory(to);
    for (auto& entry : directory_iterator_t(from)) {
      copy_file(entry, to / entry.path().filename(), and_delete);
    }
  } else {
    throw Error("file %s has an unsupported type", from);
  }

  set_write_time(to, lstat(from.string().c_str()));
  if (and_delete) {
    if (!std::filesystem::is_symlink(from_status)) {
      std::filesystem::permissions(from, std::filesystem::perms::owner_write,
                                   std::filesystem::perm_options::add |
                                       std::filesystem::perm_options::nofollow);
}
    std::filesystem::remove(from);
  }
}

void move_file(const Path& old_name, const Path& new_name) {
  try {
    std::filesystem::rename(old_name, new_name);
  } catch (std::filesystem::filesystem_error& e) {
    auto old_path = std::filesystem::path(old_name);
    auto new_path = std::filesystem::path(new_name);
    // For the move to be as atomic as possible, copy to a temporary
    // directory
    std::filesystem::path temp =
        create_temp_dir(os_string_to_string(path_view_ng_t{new_path.parent_path()}), "rename-tmp");
    finally_t remove_temp = [&]() { std::filesystem::remove(temp); };
    auto temp_copy_target = temp / "copy-target";
    if (e.code().value() == EXDEV) {
      std::filesystem::remove(new_path);
      warn("can’t rename %s as %s, copying instead", old_name, new_name);
      copy_file(old_path, temp_copy_target, true);
      std::filesystem::rename(os_string_to_string(path_view_ng_t{temp_copy_target}),
                              os_string_to_string(path_view_ng_t{new_path}));
    }
  }
}

//////////////////////////////////////////////////////////////////////

bool is_executable_file_ambient(const std::filesystem::path& exe) {
  // Check file type, because directory being executable means
  // something completely different.
  // `is_regular_file` follows symlinks before checking.
  return std::filesystem::is_regular_file(exe) && access(exe.string().c_str(),
#ifdef WIN32
                                                         0 // TODO do better
#else
                                                         X_OK
#endif
                                                         ) == 0;
}

std::filesystem::path make_parent_canonical(const std::filesystem::path& raw_path) {
  std::filesystem::path path(abs_path(raw_path));
  ;
  try {
    auto parent = path.parent_path();
    if (parent == path) {
      // `path` is a root directory => trivially canonical
      return parent;
    }
    return std::filesystem::canonical(parent) / path.filename();
  } catch (std::filesystem::filesystem_error& e) {
    throw sys_error_t("canonicalising parent path of '%1%'", path);
  }
}

bool chmod_if_needed(const std::filesystem::path& path, mode_t mode, mode_t mask) {
  auto path_string = path.string();
  auto prev_mode = lstat(path_string).st_mode;

  if (((prev_mode ^ mode) & mask) == 0) {
    return false;
}

  if (chmod(path_string.c_str(), mode) != 0) {
    throw sys_error_t("could not set permissions on '%s' to %o", path_string, mode);
}

  return true;
}

} // namespace nix
