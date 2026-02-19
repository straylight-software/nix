#include "nix/util/source-path.h"

namespace nix {

std::string_view source_path_t::base_name() const {
  return path.base_name().value_or("source");
}

source_path_t source_path_t::parent() const {
  auto p = path.parent();
  assert(p);
  return {accessor, std::move(*p)};
}

std::string source_path_t::read_file() const {
  return accessor->read_file(path);
}

bool source_path_t::path_exists() const {
  return accessor->path_exists(path);
}

SourceAccessor::stat_t source_path_t::lstat() const {
  return accessor->lstat(path);
}

std::optional<SourceAccessor::stat_t> source_path_t::maybe_lstat() const {
  return accessor->maybe_lstat(path);
}

SourceAccessor::dir_entries_t source_path_t::read_directory() const {
  return accessor->read_directory(path);
}

std::string source_path_t::read_link() const {
  return accessor->read_link(path);
}

void source_path_t::dump_path(Sink& sink, path_filter_t& filter) const {
  return accessor->dump_path(path, sink, filter);
}

std::optional<std::filesystem::path> source_path_t::get_physical_path() const {
  return accessor->get_physical_path(path);
}

std::string source_path_t::to_string() const {
  return accessor->show_path(path);
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
