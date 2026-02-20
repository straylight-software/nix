#include "nix/store/authorization-settings.h"

#include "nix/util/config-global.h"

#ifndef _WIN32
#  include <grp.h>
#  include <pwd.h>
#endif

#include <algorithm>
#include <ranges>
#include <span>

namespace nix {

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables,cert-err58-cpp)
authorization_settings_t authorization_settings;

namespace {
global_config_t::Register r_authorization_settings(&authorization_settings);
} // namespace
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables,cert-err58-cpp)

#ifndef _WIN32

namespace {

/**
 * Check if a user is a member of a specific group.
 *
 * @param user Username to check
 * @param gr_mem Group member list (null-terminated array)
 * @return true if user is a member of the group
 */
[[nodiscard]] auto match_user_in_group(std::string_view user, char** gr_mem) -> bool {
  if (gr_mem == nullptr) {
    return false;
  }
  // Count members first to create a span
  std::size_t count = 0;
  while (gr_mem[count] != nullptr) { // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    ++count;
  }
  // Use span to safely iterate
  const std::span<char*> members(gr_mem, count);
  return std::ranges::any_of(members,
                             [&user](const char* mem) { return user == std::string_view(mem); });
}
} // namespace

[[nodiscard]] auto match_user(const std::optional<std::string>& user,
                              const std::optional<std::string>& group, const strings_t& users)
    -> bool {
  // Wildcard matches all users
  if (std::ranges::find(users, "*") != users.end()) {
    return true;
  }

  // Direct username match
  if (user && std::ranges::find(users, *user) != users.end()) {
    return true;
  }

  // Group matching (entries starting with @)
  return std::ranges::any_of(users, [&](const std::string& entry) -> bool {
    if (!entry.starts_with("@")) {
      return false;
    }
    const std::string group_name = entry.substr(1);

    // Check if it's the user's primary group
    if (group && *group == group_name) {
      return true;
    }

    // Check if user is in this supplementary group
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    struct group* gr = getgrnam(group_name.c_str());
    if (gr == nullptr) {
      return false;
    }
    return user && match_user_in_group(*user, gr->gr_mem);
  });
}

#endif // _WIN32

} // namespace nix
