#include "nix/util/source-accessor.h"

#include <atomic>

namespace nix {

static std::atomic<size_t> next_number{0};

bool SourceAccessor::stat_t::is_not_nar_serialisable() {
  return this->type != t_regular && this->type != t_symlink && this->type != t_directory;
}

std::string SourceAccessor::stat_t::type_string() {
  switch (this->type) {
    case t_regular:
      return "regular";
    case t_symlink:
      return "symlink";
    case t_directory:
      return "directory";
    case t_char:
      return "character device";
    case t_block:
      return "block device";
    case t_socket:
      return "socket";
    case t_fifo:
      return "fifo";
    case t_unknown:
    default:
      return "unknown";
  }
  return "unknown";
}

SourceAccessor::SourceAccessor() : number(++next_number), display_prefix{"«unknown»"} {}

bool SourceAccessor::path_exists(const canon_path_t& path) {
  return maybe_lstat(path).has_value();
}

std::string SourceAccessor::read_file(const canon_path_t& path) {
  string_sink_t sink;
  std::optional<uint64_t> size;
  read_file(path, sink, [&](uint64_t _size) { size = _size; });
  assert(size && *size == sink.s.size());
  return std::move(sink.s);
}

void SourceAccessor::read_file(const canon_path_t& path, Sink& sink,
                              std::function<void(uint64_t)> size_callback) {
  auto s = read_file(path);
  size_callback(s.size());
  sink(s);
}

Hash SourceAccessor::hash_path(const canon_path_t& path, path_filter_t& filter, hash_algorithm_t ha) {
  hash_sink_t sink(ha);
  dump_path(path, sink, filter);
  return sink.finish().hash;
}

SourceAccessor::stat_t SourceAccessor::lstat(const canon_path_t& path) {
  if (auto st = maybe_lstat(path)) {
    return *st;
  } else {
    throw FileNotFound("path '%s' does not exist", show_path(path));
}
}

void SourceAccessor::set_path_display(std::string display_prefix, std::string display_suffix) {
  this->display_prefix = std::move(display_prefix);
  this->display_suffix = std::move(display_suffix);
}

std::string SourceAccessor::show_path(const canon_path_t& path) {
  return display_prefix + path.abs() + display_suffix;
}

canon_path_t SourceAccessor::resolve_symlinks(const canon_path_t& path, symlink_resolution_t mode) {
  auto res = canon_path_t::root;

  int links_allowed = 1024;

  std::list<std::string> todo;
  for (auto& c : path) {
    todo.push_back(std::string(c));
}

  while (!todo.empty()) {
    auto c = *todo.begin();
    todo.pop_front();
    if (c == "" || c == ".") {
      ;
    } else if (c == "..") {
      if (!res.is_root()) {
        res.pop();
}
    } else {
      res.push(c);
      if (mode == symlink_resolution_t::full || !todo.empty()) {
        if (auto st = maybe_lstat(res); st && st->type == SourceAccessor::t_symlink) {
          if (!links_allowed--) {
            throw Error("infinite symlink recursion in path '%s'", show_path(path));
}
          auto target = read_link(res);
          if (is_absolute(target)) {
            res = canon_path_t::root;
          } else {
            res.pop();
          }
          todo.splice(todo.begin(), tokenize_string<std::list<std::string>>(target, "/"));
        }
      }
    }
  }

  return res;
}

} // namespace nix
