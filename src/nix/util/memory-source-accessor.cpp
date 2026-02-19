#include "nix/util/memory-source-accessor.h"

#include "nix/util/json-utils.h"

namespace nix {

memory_source_accessor_t::file_t* memory_source_accessor_t::open(const canon_path_t& path,
                                                       std::optional<file_t> create) {
  bool hasRoot = root.has_value();

  // Special handling of root directory.
  if (path.isRoot() && !hasRoot) {
    if (create) {
      root = std::move(*create);
      return &root.value();
    }
    return nullptr;
  }

  // Root does not exist.
  if (!hasRoot)
    return nullptr;

  file_t* cur = &root.value();

  bool newF = false;

  for (std::string_view name : path) {
    auto* curDirP = std::get_if<file_t::directory_t>(&cur->raw);
    if (!curDirP)
      return nullptr;
    auto& curDir = *curDirP;

    auto i = curDir.entries.find(name);
    if (i == curDir.entries.end()) {
      if (!create)
        return nullptr;
      else {
        newF = true;
        i = curDir.entries.insert(i, {
                                         std::string{name},
                                         file_t::directory_t{},
                                     });
      }
    }
    cur = &i->second;
  }

  if (newF && create)
    *cur = std::move(*create);

  return cur;
}

std::string memory_source_accessor_t::readFile(const canon_path_t& path) {
  auto* f = open(path, std::nullopt);
  if (!f)
    throw Error("file '%s' does not exist", path);
  if (auto* r = std::get_if<file_t::Regular>(&f->raw))
    return r->contents;
  else
    throw Error("file '%s' is not a regular file", path);
}

bool memory_source_accessor_t::pathExists(const canon_path_t& path) {
  return open(path, std::nullopt);
}

template <>
SourceAccessor::stat_t memory_source_accessor_t::file_t::lstat() const {
  return std::visit(overloaded{
                        [](const Regular& r) {
                          return SourceAccessor::stat_t{
                              .type = SourceAccessor::tRegular,
                              .fileSize = r.contents.size(),
                              .isExecutable = r.executable,
                          };
                        },
                        [](const directory_t&) {
                          return SourceAccessor::stat_t{
                              .type = SourceAccessor::tDirectory,
                          };
                        },
                        [](const Symlink&) {
                          return SourceAccessor::stat_t{
                              .type = SourceAccessor::tSymlink,
                          };
                        },
                    },
                    this->raw);
}

std::optional<memory_source_accessor_t::stat_t> memory_source_accessor_t::maybeLstat(const canon_path_t& path) {
  const auto* f = open(path, std::nullopt);
  return f ? std::optional{f->lstat()} : std::nullopt;
}

memory_source_accessor_t::dir_entries_t memory_source_accessor_t::readDirectory(const canon_path_t& path) {
  auto* f = open(path, std::nullopt);
  if (!f)
    throw Error("file '%s' does not exist", path);
  if (auto* d = std::get_if<file_t::directory_t>(&f->raw)) {
    dir_entries_t res;
    for (auto& [name, file] : d->entries)
      res.insert_or_assign(name, file.lstat().type);
    return res;
  } else
    throw Error("file '%s' is not a directory", path);
  return {};
}

std::string memory_source_accessor_t::readLink(const canon_path_t& path) {
  auto* f = open(path, std::nullopt);
  if (!f)
    throw Error("file '%s' does not exist", path);
  if (auto* s = std::get_if<file_t::Symlink>(&f->raw))
    return s->target;
  else
    throw Error("file '%s' is not a symbolic link", path);
}

source_path_t memory_source_accessor_t::addFile(canon_path_t path, std::string&& contents) {
  // Create root directory automatically if necessary as a convenience.
  if (!root && !path.isRoot())
    open(canon_path_t::root, file_t::directory_t{});

  auto* f = open(path, file_t{file_t::Regular{}});
  if (!f)
    throw Error("file '%s' cannot be made because some parent file is not a directory", path);
  if (auto* r = std::get_if<file_t::Regular>(&f->raw))
    r->contents = std::move(contents);
  else
    throw Error("file '%s' is not a regular file", path);

  return source_path_t{ref(shared_from_this()), path};
}

using file_t = memory_source_accessor_t::file_t;

void memory_sink_t::createDirectory(const canon_path_t& path) {
  auto* f = dst.open(path, file_t{file_t::directory_t{}});
  if (!f)
    throw Error("file '%s' cannot be made because some parent file is not a directory", path);

  if (!std::holds_alternative<file_t::directory_t>(f->raw))
    throw Error("file '%s' is not a directory", path);
};

struct create_memory_regular_file_t : create_regular_file_sink_t {
  file_t::Regular& regularFile;

  create_memory_regular_file_t(file_t::Regular& r) : regularFile(r) {}

  void operator()(std::string_view data) override;
  void isExecutable() override;
  void preallocateContents(uint64_t size) override;
};

void memory_sink_t::createRegularFile(const canon_path_t& path,
                                   std::function<void(create_regular_file_sink_t&)> func) {
  auto* f = dst.open(path, file_t{file_t::Regular{}});
  if (!f)
    throw Error("file '%s' cannot be made because some parent file is not a directory", path);
  if (auto* rp = std::get_if<file_t::Regular>(&f->raw)) {
    create_memory_regular_file_t crf{*rp};
    func(crf);
  } else
    throw Error("file '%s' is not a regular file", path);
}

void create_memory_regular_file_t::isExecutable() {
  regularFile.executable = true;
}

void create_memory_regular_file_t::preallocateContents(uint64_t len) {
  regularFile.contents.reserve(len);
}

void create_memory_regular_file_t::operator()(std::string_view data) {
  regularFile.contents += data;
}

void memory_sink_t::createSymlink(const canon_path_t& path, const std::string& target) {
  auto* f = dst.open(path, file_t{file_t::Symlink{}});
  if (!f)
    throw Error("file '%s' cannot be made because some parent file is not a directory", path);
  if (auto* s = std::get_if<file_t::Symlink>(&f->raw))
    s->target = target;
  else
    throw Error("file '%s' is not a symbolic link", path);
}

ref<SourceAccessor> makeEmptySourceAccessor() {
  static auto empty = []() {
    auto empty = make_ref<memory_source_accessor_t>();
    memory_sink_t sink{*empty};
    sink.createDirectory(canon_path_t::root);
    /* Don't forget to clear the display prefix, as the default constructed
       SourceAccessor has the «unknown» prefix. Since this accessor is supposed
       to mimic an empty root directory the prefix needs to be empty. */
    empty->setPathDisplay("");
    return empty.cast<SourceAccessor>();
  }();
  return empty;
}

} // namespace nix
