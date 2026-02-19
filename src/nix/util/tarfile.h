#pragma once
///@file

#include <archive.h>

#include "nix/util/fs-sink.h"
#include "nix/util/serialise.h"

namespace nix {

struct tar_archive_t {
  struct archive* archive;
  Source* source;
  std::vector<unsigned char> buffer;

  void check(int err, const std::string& reason = "failed to extract archive (%s)");

  explicit tar_archive_t(const std::filesystem::path& path);

  /// @brief Create a generic archive from source.
  /// @param source - Input byte stream.
  /// @param raw - Whether to enable raw file support. For more info look in docs:
  /// https://manpages.debian.org/stretch/libarchive-dev/archive_read_format.3.en.html
  /// @param compression_method - Primary compression method to use. std::nullopt means 'all'.
  tar_archive_t(Source& source, bool raw = false,
             std::optional<std::string> compression_method = std::nullopt);

  /// Disable copy constructor. Explicitly default move assignment/constructor.
  tar_archive_t(const tar_archive_t&) = delete;
  tar_archive_t& operator=(const tar_archive_t&) = delete;
  tar_archive_t(tar_archive_t&&) = default;
  tar_archive_t& operator=(tar_archive_t&&) = default;

  void close();

  ~tar_archive_t();
};

int getArchiveFilterCodeByName(const std::string& method);

void unpackTarfile(Source& source, const std::filesystem::path& destDir);

void unpackTarfile(const std::filesystem::path& tarFile, const std::filesystem::path& destDir);

time_t unpackTarfileToSink(tar_archive_t& archive, extended_file_system_object_sink_t& parseSink);

} // namespace nix
