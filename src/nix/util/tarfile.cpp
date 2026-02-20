#include "nix/util/tarfile.h"

#include <archive.h>
#include <archive_entry.h>

#include "nix/util/file-system.h"
#include "nix/util/finally.h"
#include "nix/util/serialise.h"

namespace nix {

namespace {

int callback_open(struct archive*, void* self) {
  return ARCHIVE_OK;
}

ssize_t callback_read(struct archive* archive, void* _self, const void** buffer) {
  auto self = (tar_archive_t*)_self;
  *buffer = self->buffer.data();

  try {
    return self->source->read((char*)self->buffer.data(), self->buffer.size());
  } catch (EndOfFile&) {
    return 0;
  } catch (std::exception& err) {
    archive_set_error(archive, EIO, "source_t threw exception: %s", err.what());
    return -1;
  }
}

int callback_close(struct archive*, void* self) {
  return ARCHIVE_OK;
}

void check_lib_archive(archive* archive, int err, const std::string& reason) {
  if (err == ARCHIVE_EOF) {
    throw EndOfFile("reached end of archive");
  } else if (err != ARCHIVE_OK) {
    throw Error(reason, archive_error_string(archive));
  }
}

constexpr auto default_buffer_size = std::size_t{65536};
} // namespace

void tar_archive_t::check(int err, const std::string& reason) {
  check_lib_archive(archive, err, reason);
}

/// @brief Get filter_code from its name.
///
/// libarchive does not provide a convenience function like archive_write_add_filter_by_name but for
/// reading. Instead it's necessary to use this kludge to convert method -> code and then use
/// archive_read_support_filter_by_code. Arguably this is better than hand-rolling the equivalent
/// function that is better implemented in libarchive.
int get_archive_filter_code_by_name(const std::string& method) {
  auto* ar = archive_write_new();
  auto cleanup = finally_t{
      [&ar]() { check_lib_archive(ar, archive_write_close(ar), "failed to close archive: %s"); }};
  auto err = archive_write_add_filter_by_name(ar, method.c_str());
  check_lib_archive(ar, err, "failed to get libarchive filter by name: %s");
  auto code = archive_filter_code(ar, 0);
  return code;
}

static void enable_supported_formats(struct archive* archive) {
  archive_read_support_format_tar(archive);
  archive_read_support_format_zip(archive);

  /* Enable support for empty files so we don't throw an exception
     for empty HTTP 304 "Not modified" responses. See
     download_tarball(). */
  archive_read_support_format_empty(archive);
}

tar_archive_t::tar_archive_t(source_t& source, bool raw,
                             std::optional<std::string> compression_method)
    : archive{archive_read_new()}, source{&source}, buffer(default_buffer_size) {
  if (!compression_method) {
    archive_read_support_filter_all(archive);
  } else {
    archive_read_support_filter_by_code(archive,
                                        get_archive_filter_code_by_name(*compression_method));
  }

  if (!raw) {
    enable_supported_formats(archive);
  } else {
    archive_read_support_format_raw(archive);
    archive_read_support_format_empty(archive);
  }

  archive_read_set_option(archive, NULL, "mac-ext", NULL);
  check(archive_read_open(archive, (void*)this, callback_open, callback_read, callback_close),
        "Failed to open archive (%s)");
}

tar_archive_t::tar_archive_t(const std::filesystem::path& path)
    : archive{archive_read_new()}, buffer(default_buffer_size) {
  archive_read_support_filter_all(archive);
  enable_supported_formats(archive);
  archive_read_set_option(archive, NULL, "mac-ext", NULL);
  check(archive_read_open_filename(archive, path.string().c_str(), 16384),
        "failed to open archive: %s");
}

void tar_archive_t::close() {
  check(archive_read_close(this->archive), "Failed to close archive (%s)");
}

tar_archive_t::~tar_archive_t() {
  if (this->archive) {
    archive_read_free(this->archive);
  }
}

static void extract_archive(tar_archive_t& archive, const std::filesystem::path& dest_dir) {
  int flags =
      ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_SECURE_SYMLINKS | ARCHIVE_EXTRACT_SECURE_NODOTDOT;

  for (;;) {
    struct archive_entry* entry;
    int r = archive_read_next_header(archive.archive, &entry);
    if (r == ARCHIVE_EOF) {
      break;
    }
    auto name = archive_entry_pathname(entry);
    if (!name) {
      throw Error("cannot get archive member name: %s", archive_error_string(archive.archive));
    }
    if (r == ARCHIVE_WARN) {
      warn(archive_error_string(archive.archive));
    } else {
      archive.check(r);
    }

    archive_entry_copy_pathname(entry, (dest_dir / name).string().c_str());

    // sources can and do contain dirs with no rx bits
    if (archive_entry_filetype(entry) == AE_IFDIR && (archive_entry_mode(entry) & 0500) != 0500) {
      archive_entry_set_mode(entry, archive_entry_mode(entry) | 0500);
    }

    // Patch hardlink path
    const char* original_hardlink = archive_entry_hardlink(entry);
    if (original_hardlink) {
      archive_entry_copy_hardlink(entry, (dest_dir / original_hardlink).string().c_str());
    }

    archive.check(archive_read_extract(archive.archive, entry, flags));
  }

  archive.close();
}

void unpack_tarfile(source_t& source, const std::filesystem::path& dest_dir) {
  auto archive = tar_archive_t(source);

  create_dirs(dest_dir);
  extract_archive(archive, dest_dir);
}

void unpack_tarfile(const std::filesystem::path& tar_file, const std::filesystem::path& dest_dir) {
  auto archive = tar_archive_t(tar_file);

  create_dirs(dest_dir);
  extract_archive(archive, dest_dir);
}

time_t unpack_tarfile_to_sink(tar_archive_t& archive,
                              extended_file_system_object_sink_t& parse_sink) {
  time_t last_modified = 0;

  /* Only allocate the buffer once. use the heap because 131 KiB is a bit too
     much for the stack. */
  std::vector<unsigned char> buf(128 * 1024);

  for (;;) {
    // FIXME: merge with extract_archive
    struct archive_entry* entry;
    int r = archive_read_next_header(archive.archive, &entry);
    if (r == ARCHIVE_EOF) {
      break;
    }
    auto path = archive_entry_pathname(entry);
    if (!path) {
      throw Error("cannot get archive member name: %s", archive_error_string(archive.archive));
    }
    auto cpath = canon_path_t{path};
    if (r == ARCHIVE_WARN) {
      warn(archive_error_string(archive.archive));
    } else {
      archive.check(r);
    }

    last_modified = std::max(last_modified, archive_entry_mtime(entry));

    if (auto target = archive_entry_hardlink(entry)) {
      parse_sink.create_hardlink(cpath, canon_path_t(target));
      continue;
    }

    switch (auto type = archive_entry_filetype(entry)) {
      case AE_IFDIR:
        parse_sink.create_directory(cpath);
        break;

      case AE_IFREG: {
        parse_sink.create_regular_file(cpath, [&](auto& crf) {
          if (archive_entry_mode(entry) & S_IXUSR) {
            crf.is_executable();
          }

          while (true) {
            auto n = archive_read_data(archive.archive, buf.data(), buf.size());
            if (n < 0) {
              check_lib_archive(archive.archive, n, "cannot read file from tarball: %s");
            }
            if (n == 0) {
              break;
            }
            crf(std::string_view{
                (const char*)buf.data(),
                (size_t)n,
            });
          }
        });

        break;
      }

      case AE_IFLNK: {
        auto target = archive_entry_symlink(entry);

        parse_sink.create_symlink(cpath, target);

        break;
      }

      default:
        throw Error("file '%s' in tarball has unsupported file type %d", path, type);
    }
  }

  return last_modified;
}

} // namespace nix
