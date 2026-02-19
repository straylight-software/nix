#include "nix/util/nar-accessor.h"

#include <map>
#include <stack>

#include <nlohmann/json.hpp>

#include "nix/util/archive.h"

namespace nix {

struct nar_member_t {
  SourceAccessor::stat_t stat;

  std::string target;

  /* If this is a directory, all the children of the directory. */
  std::map<std::string, nar_member_t> children;
};

struct nar_member_constructor_t : create_regular_file_sink_t {
private:
  nar_member_t& narMember;

  uint64_t& pos;

public:
  nar_member_constructor_t(nar_member_t& nm, uint64_t& pos) : narMember(nm), pos(pos) {}

  void is_executable() override { narMember.stat.is_executable = true; }

  void preallocate_contents(uint64_t size) override {
    narMember.stat.file_size = size;
    narMember.stat.nar_offset = pos;
  }

  void operator()(std::string_view data) override {}
};

struct nar_accessor_t : public SourceAccessor {
  std::optional<const std::string> nar;

  get_nar_bytes_t get_nar_bytes;

  nar_member_t root;

  struct nar_indexer_t : file_system_object_sink_t, Source {
    nar_accessor_t& acc;
    Source& source;

    std::stack<nar_member_t*> parents;

    bool is_exec = false;

    uint64_t pos = 0;

    nar_indexer_t(nar_accessor_t& acc, Source& source) : acc(acc), source(source) {}

    nar_member_t& create_member(const canon_path_t& path, nar_member_t member) {
      size_t level = 0;
      for (auto _ : path) {
        (void)_;
        ++level;
      }

      while (parents.size() > level) {
        parents.pop();
}

      if (parents.empty()) {
        acc.root = std::move(member);
        parents.push(&acc.root);
        return acc.root;
      } else {
        if (parents.top()->stat.type != Type::t_directory) {
          throw Error("NAR file missing parent directory of path '%s'", path);
}
        auto result = parents.top()->children.emplace(*path.base_name(), std::move(member));
        auto& ref = result.first->second;
        parents.push(&ref);
        return ref;
      }
    }

    void create_directory(const canon_path_t& path) override {
      create_member(path, nar_member_t{.stat = {.type = Type::t_directory,
                                            .file_size = 0,
                                            .is_executable = false,
                                            .nar_offset = 0}});
    }

    void create_regular_file(const canon_path_t& path,
                           std::function<void(create_regular_file_sink_t&)> func) override {
      auto& nm = create_member(path, nar_member_t{.stat = {.type = Type::t_regular,
                                                       .file_size = 0,
                                                       .is_executable = false,
                                                       .nar_offset = 0}});
      nar_member_constructor_t nmc{nm, pos};
      nmc.skip_contents = true; /* Don't care about contents. */
      func(nmc);
    }

    void create_symlink(const canon_path_t& path, const std::string& target) override {
      create_member(path, nar_member_t{.stat = {.type = Type::t_symlink}, .target = target});
    }

    size_t read(char* data, size_t len) override {
      auto n = source.read(data, len);
      pos += n;
      return n;
    }
  };

  nar_accessor_t(std::string&& _nar) : nar(_nar) {
    string_source_t source(*nar);
    nar_indexer_t indexer(*this, source);
    parse_dump(indexer, indexer);
  }

  nar_accessor_t(Source& source) {
    nar_indexer_t indexer(*this, source);
    parse_dump(indexer, indexer);
  }

  nar_accessor_t(Source& source, get_nar_bytes_t get_nar_bytes) : get_nar_bytes(std::move(get_nar_bytes)) {
    nar_indexer_t indexer(*this, source);
    parse_dump(indexer, indexer);
  }

  nar_accessor_t(const nlohmann::json& listing, get_nar_bytes_t get_nar_bytes) : get_nar_bytes(get_nar_bytes) {
    [&](this const auto& recurse, nar_member_t& member, const nlohmann::json& v) -> void {
      std::string type = v["type"];

      if (type == "directory") {
        member.stat = {.type = Type::t_directory};
        for (const auto& [name, function] : v["entries"].items()) {
          recurse(member.children[name], function);
        }
      } else if (type == "regular") {
        member.stat = {.type = Type::t_regular,
                       .file_size = v["size"],
                       .is_executable = v.value("executable", false),
                       .nar_offset = v["narOffset"]};
      } else if (type == "symlink") {
        member.stat = {.type = Type::t_symlink};
        member.target = v.value("target", "");
      } else {
        return;
}
    }(root, listing);
  }

  nar_member_t* find(const canon_path_t& path) {
    nar_member_t* current = &root;

    for (const auto& i : path) {
      if (current->stat.type != Type::t_directory) {
        return nullptr;
}
      auto child = current->children.find(std::string(i));
      if (child == current->children.end()) {
        return nullptr;
}
      current = &child->second;
    }

    return current;
  }

  nar_member_t& get(const canon_path_t& path) {
    auto result = find(path);
    if (!result) {
      throw Error("NAR file does not contain path '%1%'", path);
}
    return *result;
  }

  std::optional<stat_t> maybe_lstat(const canon_path_t& path) override {
    auto i = find(path);
    if (!i) {
      return std::nullopt;
}
    return i->stat;
  }

  dir_entries_t read_directory(const canon_path_t& path) override {
    auto i = get(path);

    if (i.stat.type != Type::t_directory) {
      throw Error("path '%1%' inside NAR file is not a directory", path);
}

    dir_entries_t res;
    for (const auto& child : i.children) {
      res.insert_or_assign(child.first, std::nullopt);
}

    return res;
  }

  std::string read_file(const canon_path_t& path) override {
    auto i = get(path);
    if (i.stat.type != Type::t_regular) {
      throw Error("path '%1%' inside NAR file is not a regular file", path);
}

    if (get_nar_bytes) {
      return get_nar_bytes(*i.stat.nar_offset, *i.stat.file_size);
}

    assert(nar);
    return std::string(*nar, *i.stat.nar_offset, *i.stat.file_size);
  }

  std::string read_link(const canon_path_t& path) override {
    auto i = get(path);
    if (i.stat.type != Type::t_symlink) {
      throw Error("path '%1%' inside NAR file is not a symlink", path);
}
    return i.target;
  }
};

ref<SourceAccessor> make_nar_accessor(std::string&& nar) {
  return make_ref<nar_accessor_t>(std::move(nar));
}

ref<SourceAccessor> make_nar_accessor(Source& source) {
  return make_ref<nar_accessor_t>(source);
}

ref<SourceAccessor> make_lazy_nar_accessor(const nlohmann::json& listing, get_nar_bytes_t get_nar_bytes) {
  return make_ref<nar_accessor_t>(listing, get_nar_bytes);
}

ref<SourceAccessor> make_lazy_nar_accessor(Source& source, get_nar_bytes_t get_nar_bytes) {
  return make_ref<nar_accessor_t>(source, get_nar_bytes);
}

get_nar_bytes_t seekable_get_nar_bytes(const Path& path) {
  auto_close_fd_t fd = to_descriptor(open(path.c_str(), O_RDONLY
#ifdef O_CLOEXEC
                                                       | O_CLOEXEC
#endif
                                     ));
  if (!fd) {
    throw sys_error_t("opening NAR cache file '%s'", path);
}

  return [inner = seekable_get_nar_bytes(fd.get()), fd = make_ref<auto_close_fd_t>(std::move(fd))](
             uint64_t offset, uint64_t length) { return inner(offset, length); };
}

get_nar_bytes_t seekable_get_nar_bytes(descriptor_t fd) {
  return [fd](uint64_t offset, uint64_t length) {
    if (::lseek(from_descriptor_read_only(fd), offset, SEEK_SET) == -1) {
      throw sys_error_t("seeking in file");
}

    std::string buf(length, 0);
    read_full(fd, buf.data(), length);

    return buf;
  };
}

template <bool deep>
using list_nar_result_t = std::conditional_t<deep, nar_listing_t, shallow_nar_listing_t>;

template <bool deep>
static list_nar_result_t<deep> list_nar_impl(SourceAccessor& accessor, const canon_path_t& path) {
  auto st = accessor.lstat(path);

  switch (st.type) {
    case SourceAccessor::Type::t_regular:
      return typename list_nar_result_t<deep>::regular{
          .executable = st.is_executable,
          .contents =
              nar_listing_regular_file_t{
                  .file_size = st.file_size,
                  .nar_offset = st.nar_offset && *st.nar_offset ? st.nar_offset : std::nullopt,
              },
      };
    case SourceAccessor::Type::t_directory: {
      typename list_nar_result_t<deep>::directory_t dir;
      for (const auto& [name, type] : accessor.read_directory(path)) {
        if constexpr (deep) {
          dir.entries.emplace(name, list_nar_impl<true>(accessor, path / name));
        } else {
          dir.entries.emplace(name, fso::opaque_t{});
        }
      }
      return dir;
    }
    case SourceAccessor::Type::t_symlink:
      return typename list_nar_result_t<deep>::symlink{
          .target = accessor.read_link(path),
      };
    case SourceAccessor::Type::t_block:
    case SourceAccessor::Type::t_char:
    case SourceAccessor::Type::t_socket:
    case SourceAccessor::Type::t_fifo:
    case SourceAccessor::Type::t_unknown:
      assert(false); // cannot happen for NARs
  }
}

nar_listing_t list_nar_deep(SourceAccessor& accessor, const canon_path_t& path) {
  return list_nar_impl<true>(accessor, path);
}

shallow_nar_listing_t list_nar_shallow(SourceAccessor& accessor, const canon_path_t& path) {
  return list_nar_impl<false>(accessor, path);
}

} // namespace nix
