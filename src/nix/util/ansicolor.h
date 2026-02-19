#ifndef NIX_UTIL_ANSICOLOR_H
#define NIX_UTIL_ANSICOLOR_H

/**
 * @file
 *
 * @brief Some ANSI escape sequences.
 */

namespace nix {

constexpr const char* ansi_normal = "\e[0m";
constexpr const char* ansi_bold = "\e[1m";
constexpr const char* ansi_faint = "\e[2m";
constexpr const char* ansi_italic = "\e[3m";
constexpr const char* ansi_red = "\e[31;1m";
constexpr const char* ansi_green = "\e[32;1m";
constexpr const char* ansi_warning = "\e[35;1m";
constexpr const char* ansi_blue = "\e[34;1m";
constexpr const char* ansi_magenta = "\e[35;1m";
constexpr const char* ansi_cyan = "\e[36;1m";

} // namespace nix

// Uppercase macros for backward compatibility (needed for string literal concatenation)
// NOLINTBEGIN(cppcoreguidelines-macro-usage)
#define ANSI_NORMAL "\e[0m"
#define ANSI_BOLD "\e[1m"
#define ANSI_FAINT "\e[2m"
#define ANSI_ITALIC "\e[3m"
#define ANSI_RED "\e[31;1m"
#define ANSI_GREEN "\e[32;1m"
#define ANSI_WARNING "\e[35;1m"
#define ANSI_BLUE "\e[34;1m"
#define ANSI_MAGENTA "\e[35;1m"
#define ANSI_CYAN "\e[36;1m"
// NOLINTEND(cppcoreguidelines-macro-usage)

#endif // NIX_UTIL_ANSICOLOR_H
