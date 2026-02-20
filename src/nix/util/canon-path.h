#ifndef NIX_UTIL_CANON_PATH_H
#define NIX_UTIL_CANON_PATH_H
///@file

#include <cassert>
#include <cstddef>
#include <functional>
#include <iostream>
#include <iterator>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <boost/container_hash/hash.hpp>

#include "nix/util/error.h"

namespace nix {

make_error(BadCanonPath, Error); // NOLINT(readability-identifier-naming)

/**
 * A canonical representation of a path. It ensures the following:
 *
 * - It always starts with a slash.
 *
 * - It never ends with a slash, except if the path is "/".
 *
 * - A slash is never followed by a slash (i.e. no empty components).
 *
 * - There are no components equal to '.' or '..'.
 *
 * - It does not contain NUL bytes.
 *
 * `canon_path_t` are "virtual" Nix paths for abstract file system objects;
 * they are always Unix-style paths, regardless of what OS Nix is
 * running on. The `/` root doesn't denote the ambient host file system
 * root, but some virtual FS root.
 *
 * @note It might be useful to compare `openat(some_fd, "foo/bar")` on
 * Unix. `"foo/bar"` is a relative path because an absolute path would
 * "override" the `some_fd` directory file descriptor and escape to the
 * "system root". Conversely, Nix's abstract file operations *never* escape the
 * designated virtual file system (i.e. `SourceAccessor` or
 * `ParseSink`), so `canon_path_t` does not need an absolute/relative
 * distinction.
 *
 * @note The path does not need to correspond to an actually existing
 * path, and the path may or may not have unresolved symlinks.
 */
class canon_path_t {
  std::string path_{};

public:
  /**
   * Construct a canon path from a non-canonical path. Any '.', '..'
   * or empty components are removed.
   */
  canon_path_t(std::string_view raw);

  explicit canon_path_t(const char* raw);

  struct unchecked_t {};

  canon_path_t(unchecked_t /*unused*/, std::string path_arg) : path_(std::move(path_arg)) {}

  /**
   * Construct a canon path from a vector of elements.
   */
  canon_path_t(const std::vector<std::string>& elems);

  static const canon_path_t root;

  /**
   * If `raw` starts with a slash, return
   * `canon_path_t(raw)`. Otherwise return a `canon_path_t` representing
   * `root + "/" + raw`.
   */
  canon_path_t(std::string_view raw, const canon_path_t& root);

  [[nodiscard]] auto is_root() const -> bool { return path_.size() <= 1; }

  [[nodiscard]] explicit operator std::string_view() const { return path_; }

  [[nodiscard]] auto abs() const -> const std::string& { return path_; }

  /**
   * Like abs(), but return an empty string if this path is
   * '/'. Thus the returned string never ends in a slash.
   */
  [[nodiscard]] auto abs_or_empty() const -> const std::string& {
    const static std::string epsilon;
    return is_root() ? epsilon : path_;
  }

  [[nodiscard]] auto c_str() const -> const char* { return path_.c_str(); }

  [[nodiscard]] auto rel() const -> std::string_view { return std::string_view(path_).substr(1); }

  [[nodiscard]] auto rel_c_str() const -> const char* {
    const auto* cstr = path_.c_str();
    assert(cstr[0]); // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic) for safety if
                     // invariant is broken
    return &cstr[1]; // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  }

  class iterator_t {
    /**
     * Helper class with overloaded operator-> for "drill-down" behavior.
     * This was a "temporary" string_view doesn't have to be stored anywhere.
     */
    class pointer_proxy_t {
      std::string_view segment_;

    public:
      pointer_proxy_t(std::string_view segment_arg)
          : segment_(segment_arg) {} // NOLINT(google-explicit-constructor)

      [[nodiscard]] auto operator->() const -> const std::string_view* { return &segment_; }
    };

    std::string_view remaining_;
    size_t slash_;

  public:
    using value_type = std::string_view;
    using reference_type = const std::string_view;
    using pointer_type = pointer_proxy_t;
    using difference_type = std::ptrdiff_t;
    using iterator_category = std::forward_iterator_tag;

    /**
     * Dummy default constructor required for forward iterators. Doesn't return
     * a usable iterator.
     */
    iterator_t() : slash_(0) {}

    iterator_t(std::string_view remaining)
        : remaining_(remaining),
          slash_(remaining.find('/')) {} // NOLINT(google-explicit-constructor)

    [[nodiscard]] auto remaining() const -> std::string_view { return remaining_; }

    [[nodiscard]] auto operator==(const iterator_t& other) const -> bool {
      return remaining_.data() == other.remaining_.data();
    }

    [[nodiscard]] auto operator*() const -> reference_type { return remaining_.substr(0, slash_); }

    [[nodiscard]] auto operator->() const -> pointer_type { return {**this}; }

    auto operator++() -> iterator_t& {
      if (slash_ == std::string_view::npos) {
        remaining_ = remaining_.substr(remaining_.size());
      } else {
        remaining_ = remaining_.substr(slash_ + 1);
        slash_ = remaining_.find('/');
      }
      return *this;
    }

    [[nodiscard]] auto operator++(int) -> iterator_t {
      auto tmp = *this;
      ++*this;
      return tmp;
    }
  };

  static_assert(std::forward_iterator<iterator_t>);

  [[nodiscard]] auto begin() const -> iterator_t { return {rel()}; }

  [[nodiscard]] auto end() const -> iterator_t { return {rel().substr(path_.size() - 1)}; }

  [[nodiscard]] auto parent() const -> std::optional<canon_path_t>;

  /**
   * Remove the last component. Panics if this path is the root.
   */
  void pop();

  [[nodiscard]] auto dir_of() const -> std::optional<std::string_view> {
    if (is_root()) {
      return std::nullopt;
    }
    return std::string_view(path_).substr(0, path_.rfind('/'));
  }

  [[nodiscard]] auto base_name() const -> std::optional<std::string_view> {
    if (is_root()) {
      return std::nullopt;
    }
    return std::string_view(path_).substr(path_.rfind('/') + 1);
  }

  [[nodiscard]] auto operator==(const canon_path_t& other) const -> bool {
    return path_ == other.path_;
  }

  [[nodiscard]] auto operator!=(const canon_path_t& other) const -> bool {
    return path_ != other.path_;
  }

  /**
   * Compare paths lexicographically except that path separators
   * are sorted before any other character. That is, in the sorted order
   * a directory is always followed directly by its children. For
   * instance, 'foo' < 'foo/bar' < 'foo!'.
   */
  [[nodiscard]] auto operator<=>(const canon_path_t& other) const {
    auto iter = path_.begin();
    auto jter = other.path_.begin();
    for (; iter != path_.end() && jter != other.path_.end(); ++iter, ++jter) {
      auto c_i = *iter;
      if (c_i == '/') {
        c_i = 0;
      }
      auto c_j = *jter;
      if (c_j == '/') {
        c_j = 0;
      }
      if (auto cmp = c_i <=> c_j; cmp != 0) {
        return cmp;
      }
    }
    return static_cast<int>(iter != path_.end()) <=> static_cast<int>(jter != other.path_.end());
  }

  /**
   * Return true if `this` is equal to `parent` or a child of
   * `parent`.
   */
  [[nodiscard]] auto is_within(const canon_path_t& parent) const -> bool;

  [[nodiscard]] auto remove_prefix(const canon_path_t& prefix) const -> canon_path_t;

  /**
   * Append another path to this one.
   */
  void extend(const canon_path_t& ext);

  /**
   * Concatenate two paths.
   */
  [[nodiscard]] auto operator/(const canon_path_t& ext) const -> canon_path_t;

  /**
   * Add a path component to this one. It must not contain any slashes.
   */
  void push(std::string_view component);

  [[nodiscard]] auto operator/(std::string_view component) const -> canon_path_t;

  /**
   * Check whether access to this path is allowed, which is the case
   * if 1) `this` is within any of the `allowed` paths; or 2) any of
   * the `allowed` paths are within `this`. (The latter condition
   * ensures access to the parents of allowed paths.)
   */
  [[nodiscard]] auto is_allowed(const std::set<canon_path_t>& allowed) const -> bool;

  /**
   * Return a representation `x` of `path` relative to `this`, i.e.
   * `canon_path_t(this.make_relative(x), this) == path`.
   */
  [[nodiscard]] auto make_relative(const canon_path_t& path) const -> std::string;

  friend auto hash_value(const canon_path_t& /*canon_path*/) -> std::size_t;
};

static_assert(std::ranges::forward_range<canon_path_t>);

auto operator<<(std::ostream& stream, const canon_path_t& path) -> std::ostream&;

[[nodiscard]] inline auto hash_value(const canon_path_t& canon_path) -> std::size_t {
  const boost::hash<std::string_view> hasher;
  return hasher(canon_path.path_);
}

} // namespace nix

template <>
struct std::hash<nix::canon_path_t> {
  using is_avalanching = std::true_type;

  [[nodiscard]] auto operator()(const nix::canon_path_t& canon_path) const noexcept -> std::size_t {
    return nix::hash_value(canon_path);
  }
};

#endif // NIX_UTIL_CANON_PATH_H
