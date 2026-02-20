#include "nix/util/english.h"

#include <ostream>
#include <string_view>

namespace nix {

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
auto pluralize(std::ostream& output, unsigned int count, std::string_view single,
               std::string_view plural) -> std::ostream& {
  if (count == 1) {
    output << "1 " << single;
  } else {
    output << count << " " << plural;
  }
  return output;
}

} // namespace nix
