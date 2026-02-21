#pragma once
///@file

#include <list>
#include <map>
#include <set>
#include <string>
#include <variant>
#include <vector>

namespace nix {

using strings_t = std::list<std::string>;

/**
 * Alias to ordered std::string -> std::string map container with transparent comparator.
 *
 * Used instead of std::map<std::string, std::string> to use C++14 N3657 [1]
 * heterogenous lookup consistently across the whole codebase.
 * Transparent comparators get rid of creation of unnecessary
 * temporary variables when looking up keys by `std::string_view`
 * or C-style `const char *` strings.
 *
 * [1]: https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2013/n3657.htm
 */
using string_map_t = std::map<std::string, std::string, std::less<>>;
/**
 * Alias to an ordered map of std::string -> std::string. Uses transparent comparator.
 *
 * @see string_map_t
 */
using string_pairs_t = string_map_t;

/**
 * Alias to ordered set container with transparent comparator.
 *
 * Used instead of std::set<std::string> to use C++14 N3657 [1]
 * heterogenous lookup consistently across the whole codebase.
 * Transparent comparators get rid of creation of unnecessary
 * temporary variables when looking up keys by `std::string_view`
 * or C-style `const char *` strings.
 *
 * [1]: https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2013/n3657.htm
 */
using string_set_t = std::set<std::string, std::less<>>;

/**
 * Paths are just strings.
 */
using Path = std::string;
using path_view_t = std::string_view;
using Paths = std::list<Path>;

/**
 * Alias to an ordered set of `Path`s. Uses transparent comparator.
 *
 * @see string_set_t
 */
using path_set_t = std::set<Path, std::less<>>;

using headers_t = std::vector<std::pair<std::string, std::string>>;

/**
 * Helper class to run code at startup.
 */
template <typename T>
struct on_startup_t {
  on_startup_t(T&& val) { val(); }
};

/**
 * Wrap bools to prevent string literals (i.e. 'char *') from being
 * cast to a bool in attr_t.
 */
template <typename T>
struct explicit_t {
  T t_;

  [[nodiscard]] auto operator==(const explicit_t<T>& other) const -> bool = default;

  [[nodiscard]] auto operator<(const explicit_t<T>& other) const -> bool { return t_ < other.t_; }
};

/**
 * This wants to be a little bit like rust's Cow type.
 * Some parts of the evaluator benefit greatly from being able to reuse
 * existing allocations for strings, but have to be able to also use
 * newly allocated storage for values.
 *
 * We do not define implicit conversions, even with ref qualifiers,
 * since those can easily become ambiguous to the reader and can degrade
 * into copying behaviour we want to avoid.
 */
class backed_string_view_t {
private:
  std::variant<std::string, std::string_view> data_;

  /**
   * Needed to introduce a temporary since operator-> must return
   * a pointer. Without this we'd need to store the view object
   * even when we already own a string.
   */
  class ptr_t {
  private:
    std::string_view view_;

  public:
    explicit ptr_t(std::string_view sv) : view_(sv) {}

    [[nodiscard]] auto operator->() const -> const std::string_view* { return &view_; }
  };

public:
  backed_string_view_t(std::string&& str) : data_(std::move(str)) {}

  backed_string_view_t(std::string_view sv) : data_(sv) {}

  template <size_t N>
  backed_string_view_t(const char (&lit)[N]) : data_(std::string_view(lit)) {}

  backed_string_view_t(const backed_string_view_t&) = delete;
  auto operator=(const backed_string_view_t&) -> backed_string_view_t& = delete;

  /**
   * We only want move operations defined since the sole purpose of
   * this type is to avoid copies.
   */
  backed_string_view_t(backed_string_view_t&& other) = default;
  auto operator=(backed_string_view_t&& other) -> backed_string_view_t& = default;

  [[nodiscard]] auto is_owned() const -> bool { return std::holds_alternative<std::string>(data_); }

  [[nodiscard]] auto to_owned() && -> std::string {
    return is_owned() ? std::move(std::get<std::string>(data_))
                      : std::string(std::get<std::string_view>(data_));
  }

  [[nodiscard]] auto operator*() const -> std::string_view {
    return is_owned() ? std::get<std::string>(data_) : std::get<std::string_view>(data_);
  }

  [[nodiscard]] auto operator->() const -> ptr_t { return ptr_t(**this); }
};

} // namespace nix
