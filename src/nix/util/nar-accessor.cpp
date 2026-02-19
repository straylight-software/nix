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

  void isExecutable() override { narMember.stat.isExecutable = true; }

  void preallocateContents(uint64_t size) override {
    narMember.stat.fileSize = size;
    narMember.stat.narOffset = pos;
  }

  void operator()(std::string_view data) override {}
};

struct nar_accessor_t : public SourceAccessor {
  std::optional<const std::string> nar;

  get_nar_bytes_t getNarBytes;

  nar_member_t root;

  struct nar_indexer_t : file_system_object_sink_t, Source {
    nar_accessor_t& acc;
    Source& source;

    std::stack<nar_member_t*> parents;

    bool isExec = false;

    uint64_t pos = 0;

    nar_indexer_t(nar_accessor_t& acc, Source& source) : acc(acc), source(source) {}

    nar_member_t& createMember(const canon_path_t& path, nar_member_t member) {
      size_t level = 0;
      for (auto _ : path) {
        (void)_;
        ++level;
      }

      while (parents.size() > level)
        parents.pop();

      if (parents.empty()) {
        acc.root = std::move(member);
        parents.push(&acc.root);
        return acc.root;
      } else {
        if (parents.top()->stat.type != Type::tDirectory)
          throw Error("NAR file missing parent directory of path '%s'", path);
        auto result = parents.top()->children.emplace(*path.baseName(), std::move(member));
        auto& ref = result.first->second;
        parents.push(&ref);
        return ref;
      }
    }

    void createDirectory(const canon_path_t& path) override {
      createMember(path, nar_member_t{.stat = {.type = Type::tDirectory,
                                            .fileSize = 0,
                                            .isExecutable = false,
                                            .narOffset = 0}});
    }

    void createRegularFile(const canon_path_t& path,
                           std::function<void(create_regular_file_sink_t&)> func) override {
      auto& nm = createMember(path, nar_member_t{.stat = {.type = Type::tRegular,
                                                       .fileSize = 0,
                                                       .isExecutable = false,
                                                       .narOffset = 0}});
      nar_member_constructor_t nmc{nm, pos};
      nmc.skipContents = true; /* Don't care about contents. */
      func(nmc);
    }

    void createSymlink(const canon_path_t& path, const std::string& target) override {
      createMember(path, nar_member_t{.stat = {.type = Type::tSymlink}, .target = target});
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
    parseDump(indexer, indexer);
  }

  nar_accessor_t(Source& source) {
    nar_indexer_t indexer(*this, source);
    parseDump(indexer, indexer);
  }

  nar_accessor_t(Source& source, get_nar_bytes_t getNarBytes) : getNarBytes(std::move(getNarBytes)) {
    nar_indexer_t indexer(*this, source);
    parseDump(indexer, indexer);
  }

  nar_accessor_t(const nlohmann::json& listing, get_nar_bytes_t getNarBytes) : getNarBytes(getNarBytes) {
    [&](this const auto& recurse, nar_member_t& member, const nlohmann::json& v) -> void {
      std::string type = v["type"];

      if (type == "directory") {
        member.stat = {.type = Type::tDirectory};
        for (const auto& [name, function] : v["entries"].items()) {
          recurse(member.children[name], function);
        }
      } else if (type == "regular") {
        member.stat = {.type = Type::tRegular,
                       .fileSize = v["size"],
                       .isExecutable = v.value("executable", false),
                       .narOffset = v["narOffset"]};
      } else if (type == "symlink") {
        member.stat = {.type = Type::tSymlink};
        member.target = v.value("target", "");
      } else
        return;
    }(root, listing);
  }

  nar_member_t* find(const canon_path_t& path) {
    nar_member_t* current = &root;

    for (const auto& i : path) {
      if (current->stat.type != Type::tDirectory)
        return nullptr;
      auto child = current->children.find(std::string(i));
      if (child == current->children.end())
        return nullptr;
      current = &child->second;
    }

    return current;
  }

  nar_member_t& get(const canon_path_t& path) {
    auto result = find(path);
    if (!result)
      throw Error("NAR file does not contain path '%1%'", path);
    return *result;
  }

  std::optional<stat_t> maybeLstat(const canon_path_t& path) override {
    auto i = find(path);
    if (!i)
      return std::nullopt;
    return i->stat;
  }

  dir_entries_t readDirectory(const canon_path_t& path) override {
    auto i = get(path);

    if (i.stat.type != Type::tDirectory)
      throw Error("path '%1%' inside NAR file is not a directory", path);

    dir_entries_t res;
    for (const auto& child : i.children)
      res.insert_or_assign(child.first, std::nullopt);

    return res;
  }

  std::string readFile(const canon_path_t& path) override {
    auto i = get(path);
    if (i.stat.type != Type::tRegular)
      throw Error("path '%1%' inside NAR file is not a regular file", path);

    if (getNarBytes)
      return getNarBytes(*i.stat.narOffset, *i.stat.fileSize);

    assert(nar);
    return std::string(*nar, *i.stat.narOffset, *i.stat.fileSize);
  }

  std::string readLink(const canon_path_t& path) override {
    auto i = get(path);
    if (i.stat.type != Type::tSymlink)
      throw Error("path '%1%' inside NAR file is not a symlink", path);
    return i.target;
  }
};

ref<SourceAccessor> makeNarAccessor(std::string&& nar) {
  return make_ref<nar_accessor_t>(std::move(nar));
}

ref<SourceAccessor> makeNarAccessor(Source& source) {
  return make_ref<nar_accessor_t>(source);
}

ref<SourceAccessor> makeLazyNarAccessor(const nlohmann::json& listing, get_nar_bytes_t getNarBytes) {
  return make_ref<nar_accessor_t>(listing, getNarBytes);
}

ref<SourceAccessor> makeLazyNarAccessor(Source& source, get_nar_bytes_t getNarBytes) {
  return make_ref<nar_accessor_t>(source, getNarBytes);
}

get_nar_bytes_t seekableGetNarBytes(const Path& path) {
  auto_close_fd_t fd = toDescriptor(open(path.c_str(), O_RDONLY
#ifdef O_CLOEXEC
                                                       | O_CLOEXEC
#endif
                                     ));
  if (!fd)
    throw sys_error_t("opening NAR cache file '%s'", path);

  return [inner = seekableGetNarBytes(fd.get()), fd = make_ref<auto_close_fd_t>(std::move(fd))](
             uint64_t offset, uint64_t length) { return inner(offset, length); };
}

get_nar_bytes_t seekableGetNarBytes(descriptor_t fd) {
  return [fd](uint64_t offset, uint64_t length) {
    if (::lseek(fromDescriptorReadOnly(fd), offset, SEEK_SET) == -1)
      throw sys_error_t("seeking in file");

    std::string buf(length, 0);
    readFull(fd, buf.data(), length);

    return buf;
  };
}

template <bool deep>
using list_nar_result_t = std::conditional_t<deep, nar_listing_t, shallow_nar_listing_t>;

template <bool deep>
static list_nar_result_t<deep> listNarImpl(SourceAccessor& accessor, const canon_path_t& path) {
  auto st = accessor.lstat(path);

  switch (st.type) {
    case SourceAccessor::Type::tRegular:
      return typename list_nar_result_t<deep>::Regular{
          .executable = st.isExecutable,
          .contents =
              nar_listing_regular_file_t{
                  .fileSize = st.fileSize,
                  .narOffset = st.narOffset && *st.narOffset ? st.narOffset : std::nullopt,
              },
      };
    case SourceAccessor::Type::tDirectory: {
      typename list_nar_result_t<deep>::directory_t dir;
      for (const auto& [name, type] : accessor.readDirectory(path)) {
        if constexpr (deep) {
          dir.entries.emplace(name, listNarImpl<true>(accessor, path / name));
        } else {
          dir.entries.emplace(name, fso::opaque_t{});
        }
      }
      return dir;
    }
    case SourceAccessor::Type::tSymlink:
      return typename list_nar_result_t<deep>::Symlink{
          .target = accessor.readLink(path),
      };
    case SourceAccessor::Type::tBlock:
    case SourceAccessor::Type::tChar:
    case SourceAccessor::Type::tSocket:
    case SourceAccessor::Type::tFifo:
    case SourceAccessor::Type::tUnknown:
      assert(false); // cannot happen for NARs
  }
}

nar_listing_t listNarDeep(SourceAccessor& accessor, const canon_path_t& path) {
  return listNarImpl<true>(accessor, path);
}

shallow_nar_listing_t listNarShallow(SourceAccessor& accessor, const canon_path_t& path) {
  return listNarImpl<false>(accessor, path);
}

} // namespace nix
