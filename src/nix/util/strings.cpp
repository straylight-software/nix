#include <filesystem>
#include <sstream>
#include <string>

#include "nix/util/error.h"
#include "nix/util/os-string.h"
#include "nix/util/strings-inline.h"
#include "nix/util/util.h"

namespace nix {

template std::list<std::string> tokenize_string(std::string_view s, std::string_view separators);
template string_set_t tokenize_string(std::string_view s, std::string_view separators);
template std::vector<std::string> tokenize_string(std::string_view s, std::string_view separators);

template std::list<std::string> split_string(std::string_view s, std::string_view separators);
template string_set_t split_string(std::string_view s, std::string_view separators);
template std::vector<std::string> split_string(std::string_view s, std::string_view separators);

template std::list<os_string_t> basic_split_string(std::basic_string_view<os_char_t> s,
                                              std::basic_string_view<os_char_t> separators);

template std::string concat_strings_sep(std::string_view, const std::list<std::string>&);
template std::string concat_strings_sep(std::string_view, const string_set_t&);
template std::string concat_strings_sep(std::string_view, const std::vector<std::string>&);
template std::string concat_strings_sep(std::string_view,
                                      const boost::container::small_vector<std::string, 64>&);

typedef std::string_view strings_2[2];
template std::string concat_strings_sep(std::string_view, const strings_2&);
typedef std::string_view strings_3[3];
template std::string concat_strings_sep(std::string_view, const strings_3&);
typedef std::string_view strings_4[4];
template std::string concat_strings_sep(std::string_view, const strings_4&);

template std::string drop_empty_init_then_concat_strings_sep(std::string_view,
                                                       const std::list<std::string>&);
template std::string drop_empty_init_then_concat_strings_sep(std::string_view, const string_set_t&);
template std::string drop_empty_init_then_concat_strings_sep(std::string_view,
                                                       const std::vector<std::string>&);

/**
 * Shell split string: split a string into shell arguments, respecting quotes and backslashes.
 *
 * Used for NIX_SSHOPTS handling, which previously used `tokenize_string` and was broken by
 * Arguments that need to be passed to ssh with spaces in them.
 *
 * Read https://pubs.opengroup.org/onlinepubs/9699919799/utilities/V3_chap02.html for the
 * POSIX shell specification, which is technically what we are implementing here.
 */
std::list<std::string> shell_split_string(std::string_view s) {
  std::list<std::string> result;
  std::string current;
  bool started_current = false;
  bool escaping = false;

  auto push_current = [&]() {
    if (started_current) {
      result.push_back(current);
      current.clear();
      started_current = false;
    }
  };

  auto push_char = [&](char c) {
    current.push_back(c);
    started_current = true;
  };

  auto pop = [&]() {
    auto c = s[0];
    s.remove_prefix(1);
    return c;
  };

  auto in_double_quotes = [&]() {
    started_current = true;
    // in double quotes, escaping with backslash is only effective for $, `, ", and backslash
    while (!s.empty()) {
      auto c = pop();
      if (escaping) {
        switch (c) {
          case '$':
          case '`':
          case '"':
          case '\\':
            push_char(c);
            break;
          default:
            push_char('\\');
            push_char(c);
            break;
        }
        escaping = false;
      } else if (c == '\\') {
        escaping = true;
      } else if (c == '"') {
        return;
      } else {
        push_char(c);
      }
    }
    if (s.empty()) {
      throw Error("unterminated double quote");
    }
  };

  auto in_single_quotes = [&]() {
    started_current = true;
    while (!s.empty()) {
      auto c = pop();
      if (c == '\'') {
        return;
      }
      push_char(c);
    }
    if (s.empty()) {
      throw Error("unterminated single quote");
    }
  };

  while (!s.empty()) {
    auto c = pop();
    if (escaping) {
      push_char(c);
      escaping = false;
    } else if (c == '\\') {
      escaping = true;
    } else if (c == ' ' || c == '\t') {
      push_current();
    } else if (c == '"') {
      in_double_quotes();
    } else if (c == '\'') {
      in_single_quotes();
    } else {
      push_char(c);
    }
  }

  push_current();

  return result;
}

std::string optional_bracket(std::string_view prefix, std::string_view content,
                            std::string_view suffix) {
  if (content.empty()) {
    return "";
  }
  std::string result;
  result.reserve(prefix.size() + content.size() + suffix.size());
  result.append(prefix);
  result.append(content);
  result.append(suffix);
  return result;
}

const char* require_c_string(const std::string& s) {
  if (std::memchr(s.data(), '\0', s.size())) [[unlikely]] {
    using namespace std::string_view_literals;
    auto str = replace_strings(s, "\0"sv, "␀"sv);
    throw Error("string '%s' with null (\\0) bytes used where it's not allowed", str);
  }
  return s.c_str();
}

} // namespace nix
