#pragma once

#include "source-accessor.h"

namespace nix {

/**
 * A source accessor that just forwards every operation to another
 * accessor. This is not useful in itself but can be used as a
 * superclass for accessors that do change some operations.
 */
struct ForwardingSourceAccessor : SourceAccessor {
  ref<SourceAccessor> next;

  ForwardingSourceAccessor(ref<SourceAccessor> next) : next(next) {}

  std::string readFile(const canon_path_t& path) override { return next->readFile(path); }

  void readFile(const canon_path_t& path, Sink& sink,
                std::function<void(uint64_t)> sizeCallback) override {
    next->readFile(path, sink, sizeCallback);
  }

  std::optional<stat_t> maybeLstat(const canon_path_t& path) override { return next->maybeLstat(path); }

  dir_entries_t readDirectory(const canon_path_t& path) override { return next->readDirectory(path); }

  std::string readLink(const canon_path_t& path) override { return next->readLink(path); }

  std::string showPath(const canon_path_t& path) override { return next->showPath(path); }

  std::optional<std::filesystem::path> getPhysicalPath(const canon_path_t& path) override {
    return next->getPhysicalPath(path);
  }
};

} // namespace nix
