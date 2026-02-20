#ifndef NIX_UTIL_ENGLISH_H
#define NIX_UTIL_ENGLISH_H

#include <iosfwd>
#include <string_view>

namespace nix {

/**
 * Pluralize a given value.
 *
 * If `count == 1`, prints `1 {single}` to `output`, otherwise prints `{count} {plural}`.
 */
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
[[nodiscard]] auto pluralize(std::ostream& output, unsigned int count, std::string_view single,
                             std::string_view plural) -> std::ostream&;

} // namespace nix

#endif // NIX_UTIL_ENGLISH_H
