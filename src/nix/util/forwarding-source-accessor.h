#pragma once

#include "source-accessor.h"

namespace nix {

/**
 * A source accessor that just forwards every operation to another
 * accessor. This is not useful in itself but can be used as a
 * superclass for accessors that do change some operations.
 */
struct forwarding_source_accessor_t : source_accessor_t {
  ref<source_accessor_t> next;

  forwarding_source_accessor_t(ref<source_accessor_t> next) : next(next) {}

  std::string read_file(const canon_path_t& path) override { return next->read_file(path); }

  void read_file(const canon_path_t& path, sink_t& sink,
                std::function<void(uint64_t)> size_callback) override {
    next->read_file(path, sink, size_callback);
  }

  std::optional<stat_t> maybe_lstat(const canon_path_t& path) override { return next->maybe_lstat(path); }

  dir_entries_t read_directory(const canon_path_t& path) override { return next->read_directory(path); }

  std::string read_link(const canon_path_t& path) override { return next->read_link(path); }

  std::string show_path(const canon_path_t& path) override { return next->show_path(path); }

  std::optional<std::filesystem::path> get_physical_path(const canon_path_t& path) override {
    return next->get_physical_path(path);
  }
};

} // namespace nix
