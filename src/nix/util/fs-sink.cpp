#include "nix/util/fs-sink.h"

#include <fcntl.h>

#include "nix/util/config-global.h"
#include "nix/util/error.h"

#ifdef _WIN32
#  include <fileapi.h>

#  include "nix/util/file-path.h"
#  include "nix/util/windows-error.h"
#endif

#include "util-config-private.h"

namespace nix {

void copyRecursive(SourceAccessor& accessor, const canon_path_t& from, file_system_object_sink_t& sink,
                   const canon_path_t& to) {
  auto stat = accessor.lstat(from);

  switch (stat.type) {
    case SourceAccessor::tSymlink: {
      sink.createSymlink(to, accessor.readLink(from));
      break;
    }

    case SourceAccessor::tRegular: {
      sink.createRegularFile(to, [&](create_regular_file_sink_t& crf) {
        if (stat.isExecutable)
          crf.isExecutable();
        accessor.readFile(from, crf, [&](uint64_t size) { crf.preallocateContents(size); });
      });
      break;
    }

    case SourceAccessor::tDirectory: {
      sink.createDirectory(to, [&](file_system_object_sink_t& dirSink, const canon_path_t& relDirPath) {
        for (auto& [name, _] : accessor.readDirectory(from)) {
          copyRecursive(accessor, from / name, dirSink, relDirPath / name);
        }
      });
      break;
    }

    case SourceAccessor::tChar:
    case SourceAccessor::tBlock:
    case SourceAccessor::tSocket:
    case SourceAccessor::tFifo:
    case SourceAccessor::tUnknown:
    default:
      throw Error("file '%1%' has an unsupported type of %2%", from, stat.typeString());
  }
}

struct restore_sink_settings_t : Config {
  setting_t<bool> preallocateContents{
      this, false, "preallocate-contents",
      "Whether to preallocate files when writing objects with known size."};
};

static restore_sink_settings_t restoreSinkSettings;

static global_config_t::Register r1(&restoreSinkSettings);

static std::filesystem::path append(const std::filesystem::path& src, const canon_path_t& path) {
  auto dst = src;
  if (!path.rel().empty())
    dst /= path.rel();
  return dst;
}

#ifndef _WIN32
void restore_sink_t::createDirectory(const canon_path_t& path, directory_created_callback_t callback) {
  if (path.isRoot()) {
    createDirectory(path);
    callback(*this, path);
    return;
  }

  createDirectory(path);
  assert(dirFd); // If that's not true the above call must have thrown an exception.

  restore_sink_t dirSink{startFsync};
  dirSink.dstPath = append(dstPath, path);
  dirSink.dirFd = unix::openFileEnsureBeneathNoSymlinks(
      dirFd.get(), path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);

  if (!dirSink.dirFd)
    throw sys_error_t("opening directory '%s'", dirSink.dstPath.string());

  callback(dirSink, canon_path_t::root);
}
#endif

void restore_sink_t::createDirectory(const canon_path_t& path) {
  auto p = append(dstPath, path);

#ifndef _WIN32
  if (dirFd) {
    if (path.isRoot())
      /* Trying to create a directory that we already have a file descriptor for. */
      throw Error("path '%s' already exists", p.string());

    if (::mkdirat(dirFd.get(), path.rel_c_str(), 0777) == -1)
      throw sys_error_t("creating directory '%s'", p.string());

    return;
  }
#endif

  if (!std::filesystem::create_directory(p))
    throw Error("path '%s' already exists", p.string());

#ifndef _WIN32
  if (path.isRoot()) {
    assert(!dirFd); // Handled above

    /* Open directory for further *at operations relative to the sink root
       directory. */
    dirFd = open(p.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (!dirFd)
      throw sys_error_t("creating directory '%1%'", p.string());
  }
#endif
};

struct restore_regular_file_t : create_regular_file_sink_t {
  auto_close_fd_t fd;
  bool startFsync = false;

  ~restore_regular_file_t() {
    /* Initiate an fsync operation without waiting for the
       result. The real fsync should be run before registering a
       store path, but this is a performance optimization to allow
       the disk write to start early. */
    if (fd && startFsync)
      fd.startFsync();
  }

  void operator()(std::string_view data) override;
  void isExecutable() override;
  void preallocateContents(uint64_t size) override;
};

void restore_sink_t::createRegularFile(const canon_path_t& path,
                                    std::function<void(create_regular_file_sink_t&)> func) {
  auto p = append(dstPath, path);

  restore_regular_file_t crf;
  crf.startFsync = startFsync;
  crf.fd =
#ifdef _WIN32
      CreateFileW(p.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                  CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL)
#else
      [&]() {
        /* O_EXCL together with O_CREAT ensures symbolic links in the last
          component are not followed. */
        constexpr int flags = O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC;
        if (!dirFd)
          return ::open(p.c_str(), flags, 0666);
        return unix::openFileEnsureBeneathNoSymlinks(dirFd.get(), path, flags, 0666);
      }();
#endif
      ;
  if (!crf.fd)
    throw native_sys_error_t("creating file '%1%'", p);
  func(crf);
}

void restore_regular_file_t::isExecutable() {
  // Windows doesn't have a notion of executable file permissions we
  // care about here, right?
#ifndef _WIN32
  struct stat st;
  if (fstat(fd.get(), &st) == -1)
    throw sys_error_t("fstat");
  if (fchmod(fd.get(), st.st_mode | (S_IXUSR | S_IXGRP | S_IXOTH)) == -1)
    throw sys_error_t("fchmod");
#endif
}

void restore_regular_file_t::preallocateContents(uint64_t len) {
  if (!restoreSinkSettings.preallocateContents)
    return;

#if HAVE_POSIX_FALLOCATE
  if (len) {
    errno = posix_fallocate(fd.get(), 0, len);
    /* Note that EINVAL may indicate that the underlying
       filesystem doesn't support preallocation (e.g. on
       OpenSolaris).  Since preallocation is just an
       optimisation, ignore it. */
    if (errno && errno != EINVAL && errno != EOPNOTSUPP && errno != ENOSYS)
      throw sys_error_t("preallocating file of %1% bytes", len);
  }
#endif
}

void restore_regular_file_t::operator()(std::string_view data) {
  writeFull(fd.get(), data);
}

void restore_sink_t::createSymlink(const canon_path_t& path, const std::string& target) {
  auto p = append(dstPath, path);
#ifndef _WIN32
  if (dirFd) {
    if (::symlinkat(requireCString(target), dirFd.get(), path.rel_c_str()) == -1)
      throw sys_error_t("creating symlink from '%1%' -> '%2%'", p.string(), target);
    return;
  }
#endif
  nix::createSymlink(target, p.string());
}

void regular_file_sink_t::createRegularFile(const canon_path_t& path,
                                        std::function<void(create_regular_file_sink_t&)> func) {
  struct CRF : create_regular_file_sink_t {
    regular_file_sink_t& back;

    CRF(regular_file_sink_t& back) : back(back) {}

    void operator()(std::string_view data) override { back.sink(data); }

    void isExecutable() override {}
  } crf{*this};

  func(crf);
}

void null_file_system_object_sink_t::createRegularFile(const canon_path_t& path,
                                                 std::function<void(create_regular_file_sink_t&)> func) {
  struct : create_regular_file_sink_t {
    void operator()(std::string_view data) override {}

    void isExecutable() override {}
  } crf;

  crf.skipContents = true;

  // Even though `NullFileSystemObjectSink` doesn't do anything, it's important
  // that we call the function, to e.g. advance the parser using this
  // sink.
  func(crf);
}

} // namespace nix
