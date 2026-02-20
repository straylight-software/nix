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

void copy_recursive(source_accessor_t& accessor, const canon_path_t& from,
                    file_system_object_sink_t& sink, const canon_path_t& to) {
  auto stat = accessor.lstat(from);

  switch (stat.type) {
    case source_accessor_t::t_symlink: {
      sink.create_symlink(to, accessor.read_link(from));
      break;
    }

    case source_accessor_t::t_regular: {
      sink.create_regular_file(to, [&](create_regular_file_sink_t& crf) {
        if (stat.is_executable) {
          crf.is_executable();
        }
        accessor.read_file(from, crf, [&](uint64_t size) { crf.preallocate_contents(size); });
      });
      break;
    }

    case source_accessor_t::t_directory: {
      sink.create_directory(
          to, [&](file_system_object_sink_t& dir_sink, const canon_path_t& rel_dir_path) {
            for (auto& [name, _] : accessor.read_directory(from)) {
              copy_recursive(accessor, from / name, dir_sink, rel_dir_path / name);
            }
          });
      break;
    }

    case source_accessor_t::t_char:
    case source_accessor_t::t_block:
    case source_accessor_t::t_socket:
    case source_accessor_t::t_fifo:
    case source_accessor_t::t_unknown:
    default:
      throw Error("file '%1%' has an unsupported type of %2%", from, stat.type_string());
  }
}

struct restore_sink_settings_t : config_t {
  setting_t<bool> preallocate_contents{
      this, false, "preallocate-contents",
      "Whether to preallocate files when writing objects with known size."};
};

static restore_sink_settings_t restore_sink_settings;

static global_config_t::Register r1(&restore_sink_settings);

static std::filesystem::path append(const std::filesystem::path& src, const canon_path_t& path) {
  auto dst = src;
  if (!path.rel().empty()) {
    dst /= path.rel();
  }
  return dst;
}

#ifndef _WIN32
void restore_sink_t::create_directory(const canon_path_t& path,
                                      directory_created_callback_t callback) {
  if (path.is_root()) {
    create_directory(path);
    callback(*this, path);
    return;
  }

  create_directory(path);
  assert(dir_fd); // If that's not true the above call must have thrown an exception.

  restore_sink_t dir_sink{start_fsync};
  dir_sink.dst_path = append(dst_path, path);
  dir_sink.dir_fd = unix::open_file_ensure_beneath_no_symlinks(
      dir_fd.get(), path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);

  if (!dir_sink.dir_fd) {
    throw sys_error_t("opening directory '%s'", dir_sink.dst_path.string());
  }

  callback(dir_sink, canon_path_t::root);
}
#endif

void restore_sink_t::create_directory(const canon_path_t& path) {
  auto p = append(dst_path, path);

#ifndef _WIN32
  if (dir_fd) {
    if (path.is_root()) {
      /* Trying to create a directory that we already have a file descriptor for. */
      throw Error("path '%s' already exists", p.string());
    }

    if (::mkdirat(dir_fd.get(), path.rel_c_str(), 0777) == -1) {
      throw sys_error_t("creating directory '%s'", p.string());
    }

    return;
  }
#endif

  if (!std::filesystem::create_directory(p)) {
    throw Error("path '%s' already exists", p.string());
  }

#ifndef _WIN32
  if (path.is_root()) {
    assert(!dir_fd); // Handled above

    /* Open directory for further *at operations relative to the sink root
       directory. */
    dir_fd = open(p.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (!dir_fd) {
      throw sys_error_t("creating directory '%1%'", p.string());
    }
  }
#endif
};

struct restore_regular_file_t : create_regular_file_sink_t {
  auto_close_fd_t fd;
  bool start_fsync = false;

  ~restore_regular_file_t() {
    /* Initiate an fsync operation without waiting for the
       result. The real fsync should be run before registering a
       store path, but this is a performance optimization to allow
       the disk write to start early. */
    if (fd && start_fsync) {
      fd.start_fsync();
    }
  }

  void operator()(std::string_view data) override;
  void is_executable() override;
  void preallocate_contents(uint64_t size) override;
};

void restore_sink_t::create_regular_file(const canon_path_t& path,
                                         std::function<void(create_regular_file_sink_t&)> func) {
  auto p = append(dst_path, path);

  restore_regular_file_t crf;
  crf.start_fsync = start_fsync;
  crf.fd =
#ifdef _WIN32
      CreateFileW(p.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                  CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL)
#else
      [&]() {
        /* O_EXCL together with O_CREAT ensures symbolic links in the last
          component are not followed. */
        constexpr int flags = O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC;
        if (!dir_fd) {
          return ::open(p.c_str(), flags, 0666);
        }
        return unix::open_file_ensure_beneath_no_symlinks(dir_fd.get(), path, flags, 0666);
      }();
#endif
      ;
  if (!crf.fd) {
    throw native_sys_error_t("creating file '%1%'", p);
  }
  func(crf);
}

void restore_regular_file_t::is_executable() {
  // Windows doesn't have a notion of executable file permissions we
  // care about here, right?
#ifndef _WIN32
  struct stat st;
  if (fstat(fd.get(), &st) == -1) {
    throw sys_error_t("fstat");
  }
  if (fchmod(fd.get(), st.st_mode | (S_IXUSR | S_IXGRP | S_IXOTH)) == -1) {
    throw sys_error_t("fchmod");
  }
#endif
}

void restore_regular_file_t::preallocate_contents(uint64_t len) {
  if (!restore_sink_settings.preallocate_contents) {
    return;
  }

#if HAVE_POSIX_FALLOCATE
  if (len) {
    errno = posix_fallocate(fd.get(), 0, len);
    /* Note that EINVAL may indicate that the underlying
       filesystem doesn't support preallocation (e.g. on
       OpenSolaris).  Since preallocation is just an
       optimisation, ignore it. */
    if (errno && errno != EINVAL && errno != EOPNOTSUPP && errno != ENOSYS) {
      throw sys_error_t("preallocating file of %1% bytes", len);
    }
  }
#endif
}

void restore_regular_file_t::operator()(std::string_view data) {
  write_full(fd.get(), data);
}

void restore_sink_t::create_symlink(const canon_path_t& path, const std::string& target) {
  auto p = append(dst_path, path);
#ifndef _WIN32
  if (dir_fd) {
    if (::symlinkat(require_c_string(target), dir_fd.get(), path.rel_c_str()) == -1) {
      throw sys_error_t("creating symlink from '%1%' -> '%2%'", p.string(), target);
    }
    return;
  }
#endif
  nix::create_symlink(target, p.string());
}

void regular_file_sink_t::create_regular_file(
    const canon_path_t& path, std::function<void(create_regular_file_sink_t&)> func) {
  struct CRF : create_regular_file_sink_t {
    regular_file_sink_t& back;

    CRF(regular_file_sink_t& back) : back(back) {}

    void operator()(std::string_view data) override { back.sink_(data); }

    void is_executable() override {}
  } crf{*this};

  func(crf);
}

void null_file_system_object_sink_t::create_regular_file(
    const canon_path_t& path, std::function<void(create_regular_file_sink_t&)> func) {
  struct : create_regular_file_sink_t {
    void operator()(std::string_view data) override {}

    void is_executable() override {}
  } crf;

  crf.skip_contents = true;

  // Even though `NullFileSystemObjectSink` doesn't do anything, it's important
  // that we call the function, to e.g. advance the parser using this
  // sink.
  func(crf);
}

} // namespace nix
