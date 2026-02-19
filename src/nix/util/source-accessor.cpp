#include "nix/util/source-accessor.h"

#include <atomic>

namespace nix {

static std::atomic<size_t> nextNumber{0};

bool SourceAccessor::stat_t::isNotNARSerialisable() {
  return this->type != tRegular && this->type != tSymlink && this->type != tDirectory;
}

std::string SourceAccessor::stat_t::typeString() {
  switch (this->type) {
    case tRegular:
      return "regular";
    case tSymlink:
      return "symlink";
    case tDirectory:
      return "directory";
    case tChar:
      return "character device";
    case tBlock:
      return "block device";
    case tSocket:
      return "socket";
    case tFifo:
      return "fifo";
    case tUnknown:
    default:
      return "unknown";
  }
  return "unknown";
}

SourceAccessor::SourceAccessor() : number(++nextNumber), displayPrefix{"«unknown»"} {}

bool SourceAccessor::pathExists(const canon_path_t& path) {
  return maybeLstat(path).has_value();
}

std::string SourceAccessor::readFile(const canon_path_t& path) {
  string_sink_t sink;
  std::optional<uint64_t> size;
  readFile(path, sink, [&](uint64_t _size) { size = _size; });
  assert(size && *size == sink.s.size());
  return std::move(sink.s);
}

void SourceAccessor::readFile(const canon_path_t& path, Sink& sink,
                              std::function<void(uint64_t)> sizeCallback) {
  auto s = readFile(path);
  sizeCallback(s.size());
  sink(s);
}

Hash SourceAccessor::hashPath(const canon_path_t& path, path_filter_t& filter, hash_algorithm_t ha) {
  hash_sink_t sink(ha);
  dumpPath(path, sink, filter);
  return sink.finish().hash;
}

SourceAccessor::stat_t SourceAccessor::lstat(const canon_path_t& path) {
  if (auto st = maybeLstat(path))
    return *st;
  else
    throw FileNotFound("path '%s' does not exist", showPath(path));
}

void SourceAccessor::setPathDisplay(std::string displayPrefix, std::string displaySuffix) {
  this->displayPrefix = std::move(displayPrefix);
  this->displaySuffix = std::move(displaySuffix);
}

std::string SourceAccessor::showPath(const canon_path_t& path) {
  return displayPrefix + path.abs() + displaySuffix;
}

canon_path_t SourceAccessor::resolveSymlinks(const canon_path_t& path, symlink_resolution_t mode) {
  auto res = canon_path_t::root;

  int linksAllowed = 1024;

  std::list<std::string> todo;
  for (auto& c : path)
    todo.push_back(std::string(c));

  while (!todo.empty()) {
    auto c = *todo.begin();
    todo.pop_front();
    if (c == "" || c == ".")
      ;
    else if (c == "..") {
      if (!res.isRoot())
        res.pop();
    } else {
      res.push(c);
      if (mode == symlink_resolution_t::Full || !todo.empty()) {
        if (auto st = maybeLstat(res); st && st->type == SourceAccessor::tSymlink) {
          if (!linksAllowed--)
            throw Error("infinite symlink recursion in path '%s'", showPath(path));
          auto target = readLink(res);
          if (isAbsolute(target)) {
            res = canon_path_t::root;
          } else {
            res.pop();
          }
          todo.splice(todo.begin(), tokenizeString<std::list<std::string>>(target, "/"));
        }
      }
    }
  }

  return res;
}

} // namespace nix
