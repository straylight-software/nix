#include "nix/util/source-path.h"

namespace nix {

std::string_view source_path_t::baseName() const {
  return path.baseName().value_or("source");
}

source_path_t source_path_t::parent() const {
  auto p = path.parent();
  assert(p);
  return {accessor, std::move(*p)};
}

std::string source_path_t::readFile() const {
  return accessor->readFile(path);
}

bool source_path_t::pathExists() const {
  return accessor->pathExists(path);
}

SourceAccessor::stat_t source_path_t::lstat() const {
  return accessor->lstat(path);
}

std::optional<SourceAccessor::stat_t> source_path_t::maybeLstat() const {
  return accessor->maybeLstat(path);
}

SourceAccessor::dir_entries_t source_path_t::readDirectory() const {
  return accessor->readDirectory(path);
}

std::string source_path_t::readLink() const {
  return accessor->readLink(path);
}

void source_path_t::dumpPath(Sink& sink, path_filter_t& filter) const {
  return accessor->dumpPath(path, sink, filter);
}

std::optional<std::filesystem::path> source_path_t::getPhysicalPath() const {
  return accessor->getPhysicalPath(path);
}

std::string source_path_t::to_string() const {
  return accessor->showPath(path);
}

source_path_t source_path_t::operator/(const canon_path_t& x) const {
  return {accessor, path / x};
}

source_path_t source_path_t::operator/(std::string_view c) const {
  return {accessor, path / c};
}

bool source_path_t::operator==(const source_path_t& x) const noexcept {
  return std::tie(*accessor, path) == std::tie(*x.accessor, x.path);
}

std::strong_ordering source_path_t::operator<=>(const source_path_t& x) const noexcept {
  return std::tie(*accessor, path) <=> std::tie(*x.accessor, x.path);
}

std::ostream& operator<<(std::ostream& str, const source_path_t& path) {
  str << path.to_string();
  return str;
}

} // namespace nix
