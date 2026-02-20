#include "nix/store/authorization-settings.h"

#include "nix/util/config-global.h"

#ifndef _WIN32
#  include <grp.h>
#  include <pwd.h>
#endif

#include <algorithm>

namespace nix {

authorization_settings_t authorization_settings;

static global_config_t::Register r_authorization_settings(&authorization_settings);

#ifndef _WIN32

/**
 * Check if a user is a member of a specific group.
 *
 * @param user Username to check
 * @param gr Group structure to check membership in
 * @return true if user is a member of the group
 */
static bool match_user(std::string_view user, const struct group& gr) {
  for (char** mem = gr.gr_mem; *mem; mem++) {
    if (user == std::string_view(*mem)) {
      return true;
    }
  }
  return false;
}

bool match_user(const std::optional<std::string>& user, const std::optional<std::string>& group,
                const strings_t& users) {
  // Wildcard matches all users
  if (std::find(users.begin(), users.end(), "*") != users.end()) {
    return true;
  }

  // Direct username match
  if (user && std::find(users.begin(), users.end(), *user) != users.end()) {
    return true;
  }

  // Group matching (entries starting with @)
  for (const auto& entry : users) {
    if (entry.substr(0, 1) == "@") {
      const std::string group_name = entry.substr(1);

      // Check if it's the user's primary group
      if (group && *group == group_name) {
        return true;
      }

      // Check if user is in this supplementary group
      struct group* gr = getgrnam(group_name.c_str());
      if (gr == nullptr) {
        continue;
      }
      if (user && match_user(*user, *gr)) {
        return true;
      }
    }
  }

  return false;
}

#endif // _WIN32

} // namespace nix
