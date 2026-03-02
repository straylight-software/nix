#pragma once
///@file

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <ranges>
#include <sstream>

#include "nix/util/error.h"
#include "nix/util/logging.h"
#include "nix/util/strings.h"
#include "nix/util/types.h"

namespace nix {

void init_lib_util();

/**
 * Convert a list of strings to a null-terminated vector of `char
 * *`s. The result must not be accessed beyond the lifetime of the
 * list of strings.
 */
std::vector<char*> strings_to_char_ptrs(const strings_t& ss);

make_error(FormatError, Error);

template <class... Parts>
auto concat_strings(Parts&&... parts)
    -> std::enable_if_t<(... && std::is_convertible_v<Parts, std::string_view>), std::string> {
  std::string_view views[sizeof...(parts)] = {parts...};
  return concat_strings_sep({}, views);
}

/**
 * Add quotes around a string.
 */
inline std::string quote_string(std::string_view s, char quote = '\'') {
  std::string result;
  result.reserve(s.size() + 2);
  result += quote;
  result += s;
  result += quote;
  return result;
}

/**
 * Add quotes around a collection of strings.
 */
template <class C>
strings_t quote_strings(const C& c, char quote = '\'') {
  strings_t res;
  for (auto& s : c) {
    res.push_back(quote_string(s, quote));
  }
  return res;
}

inline strings_t quote_fs_paths(const std::set<std::filesystem::path>& paths, char quote = '\'') {
  return paths |
         std::views::transform([&](const auto& p) { return quote_string(p.string(), quote); }) |
         std::ranges::to<strings_t>();
}

/**
 * Remove trailing whitespace from a string.
 *
 * \todo return std::string_view.
 */
std::string chomp(std::string_view s);

/**
 * Remove whitespace from the start and end of a string.
 */
std::string trim(std::string_view s, std::string_view whitespace = " \n\r\t");

/**
 * Replace all occurrences of a string inside another string.
 */
std::string replace_strings(std::string s, std::string_view from, std::string_view to);

std::string rewrite_strings(std::string s, const string_map_t& rewrites);

/**
 * Parse a string into an integer.
 */
template <class N>
std::optional<N> string2_int(std::string_view s);

/**
 * Like string2_int(), but support an optional suffix 'K', 'M', 'G' or
 * 'T' denoting a binary unit prefix.
 */
template <class N>
auto string2_int_with_unit_prefix(std::string_view s) -> N {
  uint64_t multiplier = 1;
  if (!s.empty()) {
    char u = std::toupper(*s.rbegin());
    if (std::isalpha(u)) {
      if (u == 'K') {
        multiplier = 1ULL << 10;
      } else if (u == 'M') {
        multiplier = 1ULL << 20;
      } else if (u == 'G') {
        multiplier = 1ULL << 30;
      } else if (u == 'T') {
        multiplier = 1ULL << 40;
      } else {
        throw UsageError("invalid unit specifier '%1%'", u);
      }
      s.remove_suffix(1);
    }
  }
  if (auto n = string2_int<N>(s)) {
    return *n * multiplier;
  }
  throw UsageError("'%s' is not an integer", s);
}

// Base also uses 'K', because it should also displayed as KiB => 100 Bytes => 0.1 KiB
#define NIX_UTIL_SIZE_UNITS                                                                        \
  NIX_UTIL_DEFINE_SIZE_UNIT(Base, 'K')                                                             \
  NIX_UTIL_DEFINE_SIZE_UNIT(Kilo, 'K')                                                             \
  NIX_UTIL_DEFINE_SIZE_UNIT(Mega, 'M')                                                             \
  NIX_UTIL_DEFINE_SIZE_UNIT(Giga, 'G')                                                             \
  NIX_UTIL_DEFINE_SIZE_UNIT(Tera, 'T')                                                             \
  NIX_UTIL_DEFINE_SIZE_UNIT(Peta, 'P')                                                             \
  NIX_UTIL_DEFINE_SIZE_UNIT(Exa, 'E')                                                              \
  NIX_UTIL_DEFINE_SIZE_UNIT(Zetta, 'Z')                                                            \
  NIX_UTIL_DEFINE_SIZE_UNIT(Yotta, 'Y')

enum class SizeUnit {
#define NIX_UTIL_DEFINE_SIZE_UNIT(name, suffix) name,
  NIX_UTIL_SIZE_UNITS
#undef NIX_UTIL_DEFINE_SIZE_UNIT
};

constexpr inline auto size_units = std::to_array<SizeUnit>({
#define NIX_UTIL_DEFINE_SIZE_UNIT(name, suffix) SizeUnit::name,
    NIX_UTIL_SIZE_UNITS
#undef NIX_UTIL_DEFINE_SIZE_UNIT
});

SizeUnit get_size_unit(int64_t value);

/**
 * Returns the unit if all values would be rendered using the same unit
 * otherwise returns `std::nullopt`.
 */
std::optional<SizeUnit> get_common_size_unit(std::initializer_list<int64_t> values);

std::string render_size_without_unit(int64_t value, SizeUnit unit, bool align = false);

char get_size_unit_suffix(SizeUnit unit);

/**
 * Pretty-print a byte value, e.g. 12433615056 is rendered as `11.6
 * GiB`. If `align` is set, the number will be right-justified by
 * padding with spaces on the left.
 */
std::string render_size(int64_t value, bool align = false);

/**
 * Parse a string into a float.
 */
template <class N>
std::optional<N> string2_float(std::string_view s);

/**
 * Convert a little-endian integer to host order.
 */
template <typename T>
auto read_little_endian(unsigned char* p) -> T {
  T x = 0;
  for (size_t i = 0; i < sizeof(x); ++i, ++p) {
    x |= ((T)*p) << (i * 8);
  }
  return x;
}

/**
 * @return true iff `s` starts with `prefix`.
 */
auto has_prefix(std::string_view s, std::string_view prefix) -> bool;

/**
 * @return true iff `s` ends in `suffix`.
 */
auto has_suffix(std::string_view s, std::string_view suffix) -> bool;

/**
 * Convert a string to lower case.
 */
std::string to_lower(std::string s);

/**
 * Escape a string as a shell word.
 *
 * This always adds single quotes, even if escaping is not strictly necessary.
 * So both
 * - `"hello world"` -> `"'hello world'"`, which needs escaping because of the space
 * - `"echo"` -> `"'echo'"`, which doesn't need escaping
 */
std::string escape_shell_arg_always(std::string_view s);

/**
 * Exception handling in destructors: print an error message, then
 * ignore the exception.
 *
 * If you're not in a destructor, you usually want to use `ignore_exception_except_interrupt()`.
 *
 * This function might also be used in callbacks whose caller may not handle exceptions,
 * but ideally we propagate the exception using an exception_ptr in such cases.
 * See e.g. `pack_builder_context_t`
 */
void ignore_exception_in_destructor(verbosity_t lvl = lvl_error);

/**
 * Not destructor-safe.
 * Print an error message, then ignore the exception.
 * If the exception is an `Interrupted` exception, rethrow it.
 *
 * This may be used in a few places where Interrupt can't happen, but that's ok.
 */
void ignore_exception_except_interrupt(verbosity_t lvl = lvl_error);

/**
 * tree_t formatting.
 */
constexpr char tree_conn[] = "├───";
constexpr char tree_last[] = "└───";
constexpr char tree_line[] = "│   ";
constexpr char tree_null[] = "    ";

/**
 * Remove common leading whitespace from the lines in the string
 * 's'. For example, if every line is indented by at least 3 spaces,
 * then we remove 3 spaces from the start of every line.
 */
std::string strip_indentation(std::string_view s);

/**
 * Get the prefix of 's' up to and excluding the next line break (LF
 * optionally preceded by CR), and the remainder following the line
 * break.
 */
std::pair<std::string_view, std::string_view> get_line(std::string_view s);

/**
 * Get a pointer to the contents of a `std::optional` if it is set, or a
 * null pointer otherise.
 *
 * Const version.
 */
template <class T>
auto get(const std::optional<T>& opt) -> const T* {
  return opt ? &*opt : nullptr;
}

/**
 * Non-const counterpart of `const T * get(const std::optional<T>)`.
 * Takes a mutable reference, but returns a mutable pointer.
 */
template <class T>
T* get(std::optional<T>& opt) {
  return opt ? &*opt : nullptr;
}

/**
 * Get a value for the specified key from an associate container.
 */
template <class T, typename K>
auto get(const T& map, const K& key) -> const typename T::mapped_type* {
  auto i = map.find(key);
  if (i == map.end()) {
    return nullptr;
  }
  return &i->second;
}

template <class T, typename K>
auto get(T& map, const K& key) -> typename T::mapped_type* {
  auto i = map.find(key);
  if (i == map.end()) {
    return nullptr;
  }
  return &i->second;
}

/**
 * Deleted because this is use-after-free liability. Just don't pass temporaries to this overload
 * set.
 */
template <class T, typename K>
auto get(T&& map, const K& key) -> typename T::mapped_type* = delete;

template <class T>
std::optional<typename T::mapped_type> get_optional(const T& map, const typename T::key_type& key) {
  auto i = map.find(key);
  if (i == map.end()) {
    return std::nullopt;
  }
  return {i->second};
}

template <class T>
std::optional<typename T::mapped_type> get_concurrent(const T& map,
                                                      const typename T::key_type& key) {
  std::optional<typename T::mapped_type> res;
  map.cvisit(key, [&](auto& x) { res = x.second; });
  return res;
}

/**
 * Get a value for the specified key from an associate container, or a default value if the key
 * isn't present.
 */
template <class T, typename K>
auto get_or(T& map, const K& key, const typename T::mapped_type& default_value) -> const
    typename T::mapped_type& {
  auto i = map.find(key);
  if (i == map.end()) {
    return default_value;
  }
  return i->second;
}

/**
 * Deleted because this is use-after-free liability. Just don't pass temporaries to this overload
 * set.
 */
template <class T, typename K>
auto get_or(T&& map, const K& key, const typename T::mapped_type& default_value) -> const
    typename T::mapped_type& = delete;

/**
 * Remove and return the first item from a container.
 */
template <class T>
std::optional<typename T::value_type> remove_begin(T& c) {
  auto i = c.begin();
  if (i == c.end()) {
    return {};
  }
  auto v = std::move(*i);
  c.erase(i);
  return v;
}

/**
 * Remove and return the first item from a container.
 */
template <class T>
std::optional<typename T::value_type> pop(T& c) {
  if (c.empty()) {
    return {};
  }
  auto v = std::move(c.front());
  c.pop();
  return v;
}

/**
 * Append items to a container. TODO: remove this once we can use
 * C++23's `append_range()`.
 */
template <class C, typename T>
void append(C& c, std::initializer_list<T> l) {
  c.insert(c.end(), l.begin(), l.end());
}

// Forward declaration - see callback.h for full definition and Callback alias
template <typename T>
class callback;

/**
 * A RAII helper that increments a counter on construction and
 * decrements it on destruction.
 */
template <typename T>
struct maintain_count_t {
  T& counter;
  long delta;

  maintain_count_t(T& counter, long delta = 1) : counter(counter), delta(delta) {
    counter += delta;
  }

  ~maintain_count_t() { counter -= delta; }
};

/**
 * A Rust/Python-like enumerate() iterator adapter.
 */
template <std::ranges::viewable_range R>
constexpr auto enumerate(R&& range) {
  /* Not std::views::enumerate because it uses difference_type for the index. */
  return std::views::zip(std::views::iota(size_t{0}), std::forward<R>(range));
}

/**
 * C++17 std::visit boilerplate
 */
template <class... Ts>
struct overloaded : Ts... {
  using Ts::operator()...;
};
template <class... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

/**
 * Provide an addition operator between strings and string_views
 * inexplicably omitted from the standard library.
 */
inline std::string operator+(const std::string& s1, std::string_view s2) {
  std::string s;
  s.reserve(s1.size() + s2.size());
  s.append(s1);
  s.append(s2);
  return s;
}

inline std::string operator+(std::string&& s, std::string_view s2) {
  s.append(s2);
  return std::move(s);
}

inline std::string operator+(std::string_view s1, const char* s2) {
  auto s2_size = strlen(s2);
  std::string s;
  s.reserve(s1.size() + s2_size);
  s.append(s1);
  s.append(s2, s2_size);
  return s;
}

} // namespace nix
