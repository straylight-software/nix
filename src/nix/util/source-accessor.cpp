#include "nix/util/source-accessor.h"

#include <atomic>

namespace nix {

static std::atomic<size_t> next_number{0};

bool source_accessor_t::stat_t::is_not_nar_serialisable() {
  return this->type != t_regular && this->type != t_symlink && this->type != t_directory;
}

std::string source_accessor_t::stat_t::type_string() {
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

source_accessor_t::source_accessor_t() : number(++next_number), display_prefix{"«unknown»"} {}

bool source_accessor_t::path_exists(const canon_path_t& path) {
  return maybe_lstat(path).has_value();
}

std::string source_accessor_t::read_file(const canon_path_t& path) {
  string_sink_t sink;
  std::optional<uint64_t> size;
  read_file(path, sink, [&](uint64_t _size) { size = _size; });
  assert(size && *size == sink.str().size());
  return std::move(sink.str());
}

void source_accessor_t::read_file(const canon_path_t& path, sink_t& sink,
                                  std::function<void(uint64_t)> size_callback) {
  auto s = read_file(path);
  size_callback(s.size());
  sink(s);
}

Hash source_accessor_t::hash_path(const canon_path_t& path, path_filter_t& filter,
                                  hash_algorithm_t ha) {
  hash_sink_t sink(ha);
  dump_path(path, sink, filter);
  return sink.finish().hash;
}

source_accessor_t::stat_t source_accessor_t::lstat(const canon_path_t& path) {
  if (auto st = maybe_lstat(path)) {
    return *st;
  } else {
    throw FileNotFound("path '%s' does not exist", show_path(path));
  }
}

void source_accessor_t::set_path_display(std::string display_prefix, std::string display_suffix) {
  this->display_prefix = std::move(display_prefix);
  this->display_suffix = std::move(display_suffix);
}

std::string source_accessor_t::show_path(const canon_path_t& path) {
  return display_prefix + path.abs() + display_suffix;
}

canon_path_t source_accessor_t::resolve_symlinks(const canon_path_t& path,
                                                 symlink_resolution_t mode) {
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
        if (auto st = maybe_lstat(res); st && st->type == source_accessor_t::t_symlink) {
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
