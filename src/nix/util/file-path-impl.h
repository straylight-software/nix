#pragma once
/**
 * @file
 *
 * Pure (no IO) infrastructure just for defining other path types;
 * should not be used directly outside of utilities.
 */
#include <string>
#include <string_view>

namespace nix {

/**
 * Unix-style path primitives.
 *
 * Nix'result own "logical" paths are always Unix-style. So this is always
 * used for that, and additionally used for native paths on Unix.
 */
struct unix_path_trait_t {
  using char_t = char;

  using String = std::string;

  using string_view_t = std::string_view;

  constexpr static char preferred_sep = '/';

  static inline bool is_path_sep(char c) { return c == '/'; }

  static inline size_t find_path_sep(string_view_t path, size_t from = 0) {
    return path.find('/', from);
  }

  static inline size_t rfind_path_sep(string_view_t path, size_t from = string_view_t::npos) {
    return path.rfind('/', from);
  }
};

/**
 * Windows-style path primitives.
 *
 * The character type is a parameter because while windows paths rightly
 * work over UTF-16 (*) using `wchar_t`, at the current time we are
 * often manipulating them converted to UTF-8 (*) using `char`.
 *
 * (Actually neither are guaranteed to be valid unicode; both are
 * arbitrary non-0 8- or 16-bit bytes. But for characters with specifical
 * meaning like '/', '\\', ':', etc., we refer to an encoding scheme,
 * and also for sake of UIs that display paths a text.)
 */
template <class CharT0>
struct windows_path_trait_t {
  using char_t = CharT0;

  using String = std::basic_string<char_t>;

  using string_view_t = std::basic_string_view<char_t>;

  constexpr static char_t preferred_sep = '\\';

  static inline bool is_path_sep(char_t c) { return c == '/' || c == preferred_sep; }

  static size_t find_path_sep(string_view_t path, size_t from = 0) {
    size_t p1 = path.find('/', from);
    size_t p2 = path.find(preferred_sep, from);
    return p1 == String::npos ? p2 : p2 == String::npos ? p1 : std::min(p1, p2);
  }

  static size_t rfind_path_sep(string_view_t path, size_t from = String::npos) {
    size_t p1 = path.rfind('/', from);
    size_t p2 = path.rfind(preferred_sep, from);
    return p1 == String::npos ? p2 : p2 == String::npos ? p1 : std::max(p1, p2);
  }
};

template <typename char_t>
using os_path_trait_t =
#ifdef _WIN32
    windows_path_trait_t<char_t>
#else
    unix_path_trait_t
#endif
    ;

/**
 * Core pure path canonicalization algorithm.
 *
 * @param hook_component
 *   A callback which is passed two arguments,
 *   references to
 *
 *   1. the result so far
 *
 *   2. the remaining path to resolve
 *
 *   This is a chance to modify those two paths in arbitrary way, e.g. if
 *   "result" points to a symlink.
 */
template <class PathDict>
typename PathDict::String canon_path_inner(typename PathDict::string_view_t remaining,
                                           auto&& hook_component) {
  assert(remaining != "");

  typename PathDict::String result;
  result.reserve(256);

  while (true) {
    /* Skip slashes. */
    while (!remaining.empty() && PathDict::is_path_sep(remaining[0])) {
      remaining.remove_prefix(1);
    }

    if (remaining.empty()) {
      break;
    }

    auto next_comp = ({
      auto next_path_sep = PathDict::find_path_sep(remaining);
      next_path_sep == remaining.npos ? remaining : remaining.substr(0, next_path_sep);
    });

    /* Ignore `.'. */
    if (next_comp == ".") {
      remaining.remove_prefix(1);
    }

    /* If `..', delete the last component. */
    else if (next_comp == "..") {
      if (!result.empty()) {
        result.erase(PathDict::rfind_path_sep(result));
      }
      remaining.remove_prefix(2);
    }

    /* normal component; copy it. */
    else {
      result += PathDict::preferred_sep;
      if (const auto slash = PathDict::find_path_sep(remaining); slash == result.npos) {
        result += remaining;
        remaining = {};
      } else {
        result += remaining.substr(0, slash);
        remaining = remaining.substr(slash);
      }

      hook_component(result, remaining);
    }
  }

  if (result.empty()) {
    result = typename PathDict::String{PathDict::preferred_sep};
  }

  return result;
}

} // namespace nix
