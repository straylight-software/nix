#pragma once

#include <iostream>

namespace nix {

/**
 * Pluralize a given value.
 *
 * If `count == 1`, prints `1 {single}` to `output`, otherwise prints `{count} {plural}`.
 */
[[nodiscard]] auto pluralize(std::ostream& output, unsigned int count, std::string_view single,
                             std::string_view plural) -> std::ostream&;

} // namespace nix
