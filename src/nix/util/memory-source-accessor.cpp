#include "nix/util/memory-source-accessor.h"

#include "nix/util/json-utils.h"

namespace nix {

memory_source_accessor_t::file_t* memory_source_accessor_t::open(const canon_path_t& path,
                                                       std::optional<file_t> create) {
  bool has_root = root.has_value();

  // Special handling of root directory.
  if (path.is_root() && !has_root) {
    if (create) {
      root = std::move(*create);
      return &root.value();
    }
    return nullptr;
  }

  // Root does not exist.
  if (!has_root)
    return nullptr;

  file_t* cur = &root.value();

  bool new_f = false;

  for (std::string_view name : path) {
    auto* cur_dir_p = std::get_if<file_t::directory_t>(&cur->raw);
    if (!cur_dir_p)
      return nullptr;
    auto& cur_dir = *cur_dir_p;

    auto i = cur_dir.entries.find(name);
    if (i == cur_dir.entries.end()) {
      if (!create)
        return nullptr;
      else {
        new_f = true;
        i = cur_dir.entries.insert(i, {
                                         std::string{name},
                                         file_t::directory_t{},
                                     });
      }
    }
    cur = &i->second;
  }

  if (new_f && create)
    *cur = std::move(*create);

  return cur;
}

std::string memory_source_accessor_t::read_file(const canon_path_t& path) {
  auto* f = open(path, std::nullopt);
  if (!f)
    throw Error("file '%s' does not exist", path);
  if (auto* r = std::get_if<file_t::regular>(&f->raw))
    return r->contents;
  else
    throw Error("file '%s' is not a regular file", path);
}

bool memory_source_accessor_t::path_exists(const canon_path_t& path) {
  return open(path, std::nullopt);
}

template <>
SourceAccessor::stat_t memory_source_accessor_t::file_t::lstat() const {
  return std::visit(overloaded{
                        [](const regular& r) {
                          return SourceAccessor::stat_t{
                              .type = SourceAccessor::t_regular,
                              .file_size = r.contents.size(),
                              .is_executable = r.executable,
                          };
                        },
                        [](const directory_t&) {
                          return SourceAccessor::stat_t{
                              .type = SourceAccessor::t_directory,
                          };
                        },
                        [](const symlink&) {
                          return SourceAccessor::stat_t{
                              .type = SourceAccessor::t_symlink,
                          };
                        },
                    },
                    this->raw);
}

std::optional<memory_source_accessor_t::stat_t> memory_source_accessor_t::maybe_lstat(const canon_path_t& path) {
  const auto* f = open(path, std::nullopt);
  return f ? std::optional{f->lstat()} : std::nullopt;
}

memory_source_accessor_t::dir_entries_t memory_source_accessor_t::read_directory(const canon_path_t& path) {
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

std::string memory_source_accessor_t::read_link(const canon_path_t& path) {
  auto* f = open(path, std::nullopt);
  if (!f)
    throw Error("file '%s' does not exist", path);
  if (auto* s = std::get_if<file_t::symlink>(&f->raw))
    return s->target;
  else
    throw Error("file '%s' is not a symbolic link", path);
}

source_path_t memory_source_accessor_t::add_file(canon_path_t path, std::string&& contents) {
  // Create root directory automatically if necessary as a convenience.
  if (!root && !path.is_root())
    open(canon_path_t::root, file_t::directory_t{});

  auto* f = open(path, file_t{file_t::regular{}});
  if (!f)
    throw Error("file '%s' cannot be made because some parent file is not a directory", path);
  if (auto* r = std::get_if<file_t::regular>(&f->raw))
    r->contents = std::move(contents);
  else
    throw Error("file '%s' is not a regular file", path);

  return source_path_t{ref(shared_from_this()), path};
}

using file_t = memory_source_accessor_t::file_t;

void memory_sink_t::create_directory(const canon_path_t& path) {
  auto* f = dst.open(path, file_t{file_t::directory_t{}});
  if (!f)
    throw Error("file '%s' cannot be made because some parent file is not a directory", path);

  if (!std::holds_alternative<file_t::directory_t>(f->raw))
    throw Error("file '%s' is not a directory", path);
};

struct create_memory_regular_file_t : create_regular_file_sink_t {
  file_t::regular& regular_file;

  create_memory_regular_file_t(file_t::regular& r) : regular_file(r) {}

  void operator()(std::string_view data) override;
  void is_executable() override;
  void preallocate_contents(uint64_t size) override;
};

void memory_sink_t::create_regular_file(const canon_path_t& path,
                                   std::function<void(create_regular_file_sink_t&)> func) {
  auto* f = dst.open(path, file_t{file_t::regular{}});
  if (!f)
    throw Error("file '%s' cannot be made because some parent file is not a directory", path);
  if (auto* rp = std::get_if<file_t::regular>(&f->raw)) {
    create_memory_regular_file_t crf{*rp};
    func(crf);
  } else
    throw Error("file '%s' is not a regular file", path);
}

void create_memory_regular_file_t::is_executable() {
  regular_file.executable = true;
}

void create_memory_regular_file_t::preallocate_contents(uint64_t len) {
  regular_file.contents.reserve(len);
}

void create_memory_regular_file_t::operator()(std::string_view data) {
  regular_file.contents += data;
}

void memory_sink_t::create_symlink(const canon_path_t& path, const std::string& target) {
  auto* f = dst.open(path, file_t{file_t::symlink{}});
  if (!f)
    throw Error("file '%s' cannot be made because some parent file is not a directory", path);
  if (auto* s = std::get_if<file_t::symlink>(&f->raw))
    s->target = target;
  else
    throw Error("file '%s' is not a symbolic link", path);
}

ref<SourceAccessor> make_empty_source_accessor() {
  static auto empty = []() {
    auto empty = make_ref<memory_source_accessor_t>();
    memory_sink_t sink{*empty};
    sink.create_directory(canon_path_t::root);
    /* Don't forget to clear the display prefix, as the default constructed
       SourceAccessor has the «unknown» prefix. Since this accessor is supposed
       to mimic an empty root directory the prefix needs to be empty. */
    empty->set_path_display("");
    return empty.cast<SourceAccessor>();
  }();
  return empty;
}

} // namespace nix
