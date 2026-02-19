#pragma once
///@file

#include "nix/util/fs-sink.h"
#include "nix/util/json-impls.h"
#include "nix/util/source-path.h"
#include "nix/util/variant-wrapper.h"

namespace nix {

/**
 * file_t System Object definitions
 *
 * @see https://nix.dev/manual/nix/latest/store/file-system-object.html
 */
namespace fso {

template <typename RegularContents>
struct regular {
  bool executable = false;
  RegularContents contents;

  auto operator<=>(const regular&) const = default;
};

/**
 * Child parameter because sometimes we want "shallow" directories without
 * full file children.
 */
template <typename Child>
struct directory_t {
  using Name = std::string;

  std::map<Name, Child, std::less<>> entries;

  inline bool operator==(const directory_t&) const noexcept;
  inline std::strong_ordering operator<=>(const directory_t&) const noexcept;
};

struct symlink {
  std::string target;

  auto operator<=>(const symlink&) const = default;
};

/**
 * For when we know there is child, but don't know anything about it.
 *
 * This is not part of the core file_t System Object data model --- this
 * represents not knowing, not an additional type of file.
 */
struct opaque_t {
  auto operator<=>(const opaque_t&) const = default;
};

/**
 * `file_t<std::string>` nicely defining what a "file system object"
 * is in Nix.
 *
 * With a different type arugment, it is also can be a "skeletal"
 * version is that abstract syntax for a "NAR listing".
 */
template <typename RegularContents, bool recur>
struct variant_t {
  bool operator==(const variant_t&) const noexcept;
  std::strong_ordering operator<=>(const variant_t&) const noexcept;

  using regular = nix::fso::regular<RegularContents>;

  /**
   * In the default case, we do want full file children for our directory.
   */
  using directory_t = nix::fso::directory_t<std::conditional_t<recur, variant_t, opaque_t>>;

  using symlink = nix::fso::symlink;

  using raw_t = std::variant<regular, directory_t, symlink>;
  raw_t raw;

  MAKE_WRAPPER_CONSTRUCTOR(variant_t);

  SourceAccessor::stat_t lstat() const;
};

template <typename Child>
inline bool directory_t<Child>::operator==(const directory_t&) const noexcept = default;

template <typename Child>
inline std::strong_ordering
directory_t<Child>::operator<=>(const directory_t&) const noexcept = default;

template <typename RegularContents, bool recur>
inline bool variant_t<RegularContents, recur>::operator==(
    const variant_t<RegularContents, recur>&) const noexcept = default;

template <typename RegularContents, bool recur>
inline std::strong_ordering variant_t<RegularContents, recur>::operator<=>(
    const variant_t<RegularContents, recur>&) const noexcept = default;

} // namespace fso

/**
 * An source accessor for an in-memory file system.
 */
struct memory_source_accessor_t : virtual SourceAccessor {
  using file_t = fso::variant_t<std::string, true>;

  std::optional<file_t> root;

  bool operator==(const memory_source_accessor_t&) const noexcept = default;

  bool operator<(const memory_source_accessor_t& other) const noexcept { return root < other.root; }

  std::string read_file(const canon_path_t& path) override;
  bool path_exists(const canon_path_t& path) override;
  std::optional<stat_t> maybe_lstat(const canon_path_t& path) override;
  dir_entries_t read_directory(const canon_path_t& path) override;
  std::string read_link(const canon_path_t& path) override;

  /**
   * @param create If present, create this file and any parent directories
   * that are needed.
   *
   * Return null if
   *
   * - `create = false`: file_t does not exist.
   *
   * - `create = true`: some parent file was not a dir, so couldn't
   *   look/create inside.
   */
  file_t* open(const canon_path_t& path, std::optional<file_t> create);

  source_path_t add_file(canon_path_t path, std::string&& contents);
};

/**
 * Write to a `memory_source_accessor_t` at the given path
 */
struct memory_sink_t : file_system_object_sink_t {
  memory_source_accessor_t& dst;

  memory_sink_t(memory_source_accessor_t& dst) : dst(dst) {}

  void create_directory(const canon_path_t& path) override;

  void create_regular_file(const canon_path_t& path,
                         std::function<void(create_regular_file_sink_t&)>) override;

  void create_symlink(const canon_path_t& path, const std::string& target) override;
};

template <>
struct json_avoids_null<memory_source_accessor_t::file_t::regular> : std::true_type {};

template <>
struct json_avoids_null<memory_source_accessor_t::file_t::directory_t> : std::true_type {};

template <>
struct json_avoids_null<memory_source_accessor_t::file_t::symlink> : std::true_type {};

template <>
struct json_avoids_null<memory_source_accessor_t::file_t> : std::true_type {};

template <>
struct json_avoids_null<memory_source_accessor_t> : std::true_type {};

} // namespace nix

namespace nlohmann {

using namespace nix;

#define ARG fso::regular<RegularContents>
template <typename RegularContents>
JSON_IMPL_INNER(ARG);
#undef ARG

#define ARG fso::directory_t<Child>
template <typename Child>
JSON_IMPL_INNER(ARG);
#undef ARG

template <>
JSON_IMPL_INNER(fso::symlink);

template <>
JSON_IMPL_INNER(fso::opaque_t);

#define ARG fso::variant_t<RegularContents, recur>
template <typename RegularContents, bool recur>
JSON_IMPL_INNER(ARG);
#undef ARG

} // namespace nlohmann

JSON_IMPL(memory_source_accessor_t)
