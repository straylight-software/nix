#pragma once
///@file

#include <optional>
#include <string>
#include <string_view>

namespace nix {

/**
 * Named because it is similar to the Rust type, except it is in the
 * native encoding not WTF-8.
 *
 * Same as `std::filesystem::path::value_type`, but manually defined to
 * avoid including a much more complex header.
 */
using os_char_t =
#if defined(_WIN32) && !defined(__CYGWIN__)
    wchar_t
#else
    char
#endif
    ;

/**
 * Named because it is similar to the Rust type, except it is in the
 * native encoding not WTF-8.
 *
 * Same as `std::filesystem::path::string_type`, but manually defined
 * for the same reason as `os_char_t`.
 */
using os_string_t = std::basic_string<os_char_t>;

/**
 * `std::string_view` counterpart for `os_string_t`.
 */
using os_string_view_t = std::basic_string_view<os_char_t>;

std::string os_string_to_string(os_string_view_t path);

os_string_t string_to_os_string(std::string_view s);

/**
 * Create string literals with the native character width of paths
 */
#ifndef _WIN32
#  define OS_STR(s) s
#else
#  define OS_STR(s) L##s
#endif

} // namespace nix
