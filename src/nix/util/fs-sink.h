#pragma once
///@file

#include "nix/util/file-system.h"
#include "nix/util/serialise.h"
#include "nix/util/source-accessor.h"

namespace nix {

/**
 * Actions on an open regular file in the process of creating it.
 *
 * See `file_system_object_sink_t::create_regular_file`.
 */
struct create_regular_file_sink_t : virtual Sink {
  /**
   * If set to true, the sink will not be called with the contents
   * of the file. `preallocate_contents()` will still be called to
   * convey the file size. Useful for sinks that want to efficiently
   * discard the contents of the file.
   */
  bool skip_contents = false;

  virtual void is_executable() = 0;

  /**
   * An optimization. By default, do nothing.
   */
  virtual void preallocate_contents(uint64_t size) {};
};

struct file_system_object_sink_t {
  virtual ~file_system_object_sink_t() = default;

  virtual void create_directory(const canon_path_t& path) = 0;

  using directory_created_callback_t =
      std::function<void(file_system_object_sink_t& dir_sink, const canon_path_t& dir_rel_path)>;

  /**
   * Create a directory and invoke a callback with a pair of sink + canon_path_t
   * of the created subdirectory relative to dir_sink.
   *
   * @note This allows for UNIX restore_sink_t implementations to implement
   * *at-style accessors that always keep an open file descriptor for the
   * freshly created directory. use this when it's important to disallow any
   * intermediate path components from being symlinks.
   */
  virtual void create_directory(const canon_path_t& path, directory_created_callback_t callback) {
    create_directory(path);
    callback(*this, path);
  }

  /**
   * This function in general is no re-entrant. Only one file can be
   * written at a time.
   */
  virtual void create_regular_file(const canon_path_t& path,
                                   std::function<void(create_regular_file_sink_t&)>) = 0;

  virtual void create_symlink(const canon_path_t& path, const std::string& target) = 0;
};

/**
 * An extension of `file_system_object_sink_t` that supports file types
 * that are not supported by Nix's FSO model.
 */
struct extended_file_system_object_sink_t : virtual file_system_object_sink_t {
  /**
   * Create a hard link. The target must be the path of a previously
   * encountered file relative to the root of the FSO.
   */
  virtual void create_hardlink(const canon_path_t& path, const canon_path_t& target) = 0;
};

/**
 * Recursively copy file system objects from the source into the sink.
 */
void copy_recursive(SourceAccessor& accessor, const canon_path_t& source_path,
                    file_system_object_sink_t& sink, const canon_path_t& dest_path);

/**
 * Ignore everything and do nothing
 */
struct null_file_system_object_sink_t : file_system_object_sink_t {
  void create_directory(const canon_path_t& /*path*/) override {}

  void create_symlink(const canon_path_t& /*path*/, const std::string& target) override {}

  void create_regular_file(const canon_path_t& path,
                           std::function<void(create_regular_file_sink_t&)>) override;
};

/**
 * Write files at the given path
 */
struct restore_sink_t : file_system_object_sink_t {
  std::filesystem::path dst_path{};
#ifndef _WIN32
  /**
   * file_t descriptor for the directory located at dst_path. Used for *at
   * operations relative to this file descriptor. This sink must *never*
   * follow intermediate symlinks (starting from dst_path) in case a file
   * collision is encountered for various reasons like case-insensitivity or
   * other types on normalization. using appropriate *at system calls and traversing
   * only one path component at a time ensures that writing is race-free and is
   * is not susceptible to symlink replacement.
   */
  auto_close_fd_t dir_fd;
#endif
  bool start_fsync = false;

  explicit restore_sink_t(bool start_fsync) : start_fsync{start_fsync} {}

  void create_directory(const canon_path_t& path) override;

#ifndef _WIN32
  void create_directory(const canon_path_t& path, directory_created_callback_t callback) override;
#endif

  void create_regular_file(const canon_path_t& path,
                           std::function<void(create_regular_file_sink_t&)>) override;

  void create_symlink(const canon_path_t& path, const std::string& target) override;
};

/**
 * Restore a single file at the top level, passing along
 * `receiveContents` to the underlying `Sink`. For anything but a single
 * file, set `regular = true` so the caller can fail accordingly.
 */
struct regular_file_sink_t : file_system_object_sink_t {
  bool regular = true;
  Sink& sink_;

  regular_file_sink_t(Sink& s) : sink_(s) {}

  void create_directory(const canon_path_t& /*path*/) override { regular = false; }

  void create_symlink(const canon_path_t& /*path*/, const std::string& target) override {
    regular = false;
  }

  void create_regular_file(const canon_path_t& path,
                           std::function<void(create_regular_file_sink_t&)>) override;
};

} // namespace nix
