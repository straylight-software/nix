#pragma once
///@file

#include <cassert>
#include <iostream>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <vector>

#include <boost/container_hash/hash.hpp>

#include "nix/util/error.h"

namespace nix {

MakeError(BadCanonPath, Error);

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
  std::string path;

public:
  /**
   * Construct a canon path from a non-canonical path. Any '.', '..'
   * or empty components are removed.
   */
  canon_path_t(std::string_view raw);

  explicit canon_path_t(const char* raw);

  struct unchecked_t {};

  canon_path_t(unchecked_t _, std::string path) : path(std::move(path)) {}

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

  bool isRoot() const { return path.size() <= 1; }

  explicit operator std::string_view() const { return path; }

  const std::string& abs() const { return path; }

  /**
   * Like abs(), but return an empty string if this path is
   * '/'. Thus the returned string never ends in a slash.
   */
  const std::string& absOrEmpty() const {
    const static std::string epsilon;
    return isRoot() ? epsilon : path;
  }

  const char* c_str() const { return path.c_str(); }

  std::string_view rel() const { return ((std::string_view)path).substr(1); }

  const char* rel_c_str() const {
    auto cs = path.c_str();
    assert(cs[0]); // for safety if invariant is broken
    return &cs[1];
  }

  class Iterator {
    /**
     * Helper class with overloaded operator-> for "drill-down" behavior.
     * This was a "temporary" string_view doesn't have to be stored anywhere.
     */
    class pointer_proxy_t {
      std::string_view segment;

    public:
      pointer_proxy_t(std::string_view segment_) : segment(segment_) {}

      const std::string_view* operator->() const { return &segment; }
    };

  public:
    using value_type = std::string_view;
    using reference_type = const std::string_view;
    using pointer_type = pointer_proxy_t;
    using difference_type = std::ptrdiff_t;
    using iterator_category = std::forward_iterator_tag;

    std::string_view remaining;
    size_t slash;

    /**
     * Dummy default constructor required for forward iterators. Doesn't return
     * a usable iterator.
     */
    Iterator() : remaining(), slash(0) {}

    Iterator(std::string_view remaining) : remaining(remaining), slash(remaining.find('/')) {}

    bool operator==(const Iterator& x) const { return remaining.data() == x.remaining.data(); }

    reference_type operator*() const { return remaining.substr(0, slash); }

    pointer_type operator->() const { return pointer_proxy_t(**this); }

    Iterator& operator++() {
      if (slash == remaining.npos)
        remaining = remaining.substr(remaining.size());
      else {
        remaining = remaining.substr(slash + 1);
        slash = remaining.find('/');
      }
      return *this;
    }

    Iterator operator++(int) {
      auto tmp = *this;
      ++*this;
      return tmp;
    }
  };

  static_assert(std::forward_iterator<Iterator>);

  Iterator begin() const { return Iterator(rel()); }

  Iterator end() const { return Iterator(rel().substr(path.size() - 1)); }

  std::optional<canon_path_t> parent() const;

  /**
   * Remove the last component. Panics if this path is the root.
   */
  void pop();

  std::optional<std::string_view> dirOf() const {
    if (isRoot())
      return std::nullopt;
    return ((std::string_view)path).substr(0, path.rfind('/'));
  }

  std::optional<std::string_view> baseName() const {
    if (isRoot())
      return std::nullopt;
    return ((std::string_view)path).substr(path.rfind('/') + 1);
  }

  bool operator==(const canon_path_t& x) const { return path == x.path; }

  bool operator!=(const canon_path_t& x) const { return path != x.path; }

  /**
   * Compare paths lexicographically except that path separators
   * are sorted before any other character. That is, in the sorted order
   * a directory is always followed directly by its children. For
   * instance, 'foo' < 'foo/bar' < 'foo!'.
   */
  auto operator<=>(const canon_path_t& x) const {
    auto i = path.begin();
    auto j = x.path.begin();
    for (; i != path.end() && j != x.path.end(); ++i, ++j) {
      auto c_i = *i;
      if (c_i == '/')
        c_i = 0;
      auto c_j = *j;
      if (c_j == '/')
        c_j = 0;
      if (auto cmp = c_i <=> c_j; cmp != 0)
        return cmp;
    }
    return (i != path.end()) <=> (j != x.path.end());
  }

  /**
   * Return true if `this` is equal to `parent` or a child of
   * `parent`.
   */
  bool isWithin(const canon_path_t& parent) const;

  canon_path_t removePrefix(const canon_path_t& prefix) const;

  /**
   * Append another path to this one.
   */
  void extend(const canon_path_t& x);

  /**
   * Concatenate two paths.
   */
  canon_path_t operator/(const canon_path_t& x) const;

  /**
   * Add a path component to this one. It must not contain any slashes.
   */
  void push(std::string_view c);

  canon_path_t operator/(std::string_view c) const;

  /**
   * Check whether access to this path is allowed, which is the case
   * if 1) `this` is within any of the `allowed` paths; or 2) any of
   * the `allowed` paths are within `this`. (The latter condition
   * ensures access to the parents of allowed paths.)
   */
  bool isAllowed(const std::set<canon_path_t>& allowed) const;

  /**
   * Return a representation `x` of `path` relative to `this`, i.e.
   * `canon_path_t(this.makeRelative(x), this) == path`.
   */
  std::string makeRelative(const canon_path_t& path) const;

  friend std::size_t hash_value(const canon_path_t&);
};

static_assert(std::ranges::forward_range<canon_path_t>);

std::ostream& operator<<(std::ostream& stream, const canon_path_t& path);

inline std::size_t hash_value(const canon_path_t& path) {
  boost::hash<std::string_view> hasher;
  return hasher(path.path);
}

} // namespace nix

template <>
struct std::hash<nix::canon_path_t> {
  using is_avalanching = std::true_type;

  std::size_t operator()(const nix::canon_path_t& path) const noexcept {
    return nix::hash_value(path);
  }
};
