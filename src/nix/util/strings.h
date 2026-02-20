#pragma once

#include <list>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <boost/container/small_vector.hpp>

#include "nix/util/types.h"

namespace nix {

/**
 * String tokenizer.
 *
 * See also `basic_split_string()`, which preserves empty strings between separators, as well as at
 * the start and end.
 */
template <class C, class char_t = char>
[[nodiscard]] auto basic_tokenize_string(std::basic_string_view<char_t> str,
                                         std::basic_string_view<char_t> separators) -> C;

/**
 * Like `basic_tokenize_string` but specialized to the default `char`
 */
template <class C>
C tokenize_string(std::string_view s, std::string_view separators = " \t\n\r");

extern template std::list<std::string> tokenize_string(std::string_view s,
                                                       std::string_view separators);
extern template string_set_t tokenize_string(std::string_view s, std::string_view separators);
extern template std::vector<std::string> tokenize_string(std::string_view s,
                                                         std::string_view separators);

/**
 * Split a string, preserving empty strings between separators, as well as at the start and end.
 *
 * Returns a non-empty collection of strings.
 */
template <class C, class char_t = char>
C basic_split_string(std::basic_string_view<char_t> s, std::basic_string_view<char_t> separators);
template <typename C>
C split_string(std::string_view s, std::string_view separators);

extern template std::list<std::string> split_string(std::string_view s,
                                                    std::string_view separators);
extern template string_set_t split_string(std::string_view s, std::string_view separators);
extern template std::vector<std::string> split_string(std::string_view s,
                                                      std::string_view separators);

/**
 * Concatenate the given strings with a separator between the elements.
 */
template <class C>
std::string concat_strings_sep(std::string_view sep, const C& ss);

extern template std::string concat_strings_sep(std::string_view, const std::list<std::string>&);
extern template std::string concat_strings_sep(std::string_view, const string_set_t&);
extern template std::string concat_strings_sep(std::string_view, const std::vector<std::string>&);
extern template std::string
concat_strings_sep(std::string_view, const boost::container::small_vector<std::string, 64>&);

/**
 * Apply a function to the `iterable`'s items and concat them with `separator`.
 */
template <class C, class F>
std::string concat_map_strings_sep(std::string_view separator, const C& iterable, F fn) {
  boost::container::small_vector<std::string, 64> strings;
  strings.reserve(iterable.size());
  for (const auto& elem : iterable) {
    strings.push_back(fn(elem));
  }
  return concat_strings_sep(separator, strings);
}

/**
 * Ignore any empty strings at the start of the list, and then concatenate the
 * given strings with a separator between the elements.
 *
 * @deprecated This function exists for historical reasons. You probably just
 *             want to use `concat_strings_sep`.
 */
template <class C>
[[deprecated("Consider removing the empty string dropping behavior. If acceptable, use "
             "concatStringsSep instead.")]] std::string
drop_empty_init_then_concat_strings_sep(std::string_view sep, const C& ss);

extern template std::string drop_empty_init_then_concat_strings_sep(std::string_view,
                                                                    const std::list<std::string>&);
extern template std::string drop_empty_init_then_concat_strings_sep(std::string_view,
                                                                    const string_set_t&);
extern template std::string
drop_empty_init_then_concat_strings_sep(std::string_view, const std::vector<std::string>&);

/**
 * Shell split string: split a string into shell arguments, respecting quotes and backslashes.
 *
 * Used for NIX_SSHOPTS handling, which previously used `tokenize_string` and was broken by
 * Arguments that need to be passed to ssh with spaces in them.
 */
std::list<std::string> shell_split_string(std::string_view s);

/**
 * Conditionally wrap a string with prefix and suffix brackets.
 *
 * If `content` is empty, returns an empty string.
 * Otherwise, returns `prefix + content + suffix`.
 *
 * Example:
 *   optional_bracket(" (", "foo", ")") == " (foo)"
 *   optional_bracket(" (", "", ")") == ""
 *
 * Design note: this would have been called `optionalParentheses`, except this
 * function is more general and more explicit. Parentheses typically *also* need
 * to be prefixed with a space in order to fit nicely in a piece of natural
 * language.
 */
std::string optional_bracket(std::string_view prefix, std::string_view content,
                             std::string_view suffix);

/**
 * Overload for optional content.
 *
 * If `content` is nullopt or contains an empty string, returns an empty string.
 * Otherwise, returns `prefix + *content + suffix`.
 *
 * Example:
 *   optional_bracket(" (", std::optional<std::string>("foo"), ")") == " (foo)"
 *   optional_bracket(" (", std::nullopt, ")") == ""
 *   optional_bracket(" (", std::optional<std::string>(""), ")") == ""
 */
template <typename T>
  requires std::convertible_to<T, std::string_view>
std::string optional_bracket(std::string_view prefix, const std::optional<T>& content,
                             std::string_view suffix) {
  if (!content || std::string_view(*content).empty()) {
    return "";
  }
  return optional_bracket(prefix, std::string_view(*content), suffix);
}

/**
 * Hash implementation that can be used for zero-copy heterogenous lookup from
 * P1690R1[1] in unordered containers.
 *
 * [1]: https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2019/p1690r1.html
 */
struct string_view_hash_t {
private:
  using hash_type_t = std::hash<std::string_view>;

public:
  using is_transparent = void;

  auto operator()(const char* str) const {
    /* This has a slight overhead due to an implicit strlen, but there isn't
       a good way around it because the hash value of all overloads must be
       consistent. Delegating to string_view is the solution initially proposed
       in P0919R3. */
    return hash_type_t{}(std::string_view{str});
  }

  auto operator()(std::string_view str) const { return hash_type_t{}(str); }

  auto operator()(const std::string& str) const { return hash_type_t{}(std::string_view{str}); }
};

/**
 * Check that the string does not contain any NUL bytes and return c_str().
 * @throws Error if str contains '\0' bytes.
 */
const char* require_c_string(const std::string& str);

} // namespace nix
