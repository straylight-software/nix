#pragma once
///@file
/**
 * Settings related to authenticating clients for the Nix daemon.
 *
 * For pipes we have little good information about the client side, but
 * for Unix domain sockets we do. So currently these options implement
 * mandatory access control based on user names and group names (looked
 * up and translated to UID/GIDs in the daemon process).
 */

#include "nix/util/configuration.h"
#include "nix/util/types.h"

namespace nix {

struct authorization_settings_t : config_t {
  setting_t<strings_t> trusted_users{this,
                                     {"root"},
                                     "trusted-users",
                                     R"(
          A list of user names, separated by whitespace.
          These users will have additional rights when connecting to the Nix daemon, such as the ability to specify additional [substituters](#conf-substituters), or to import unsigned realisations or unsigned input-addressed store objects.

          You can also specify groups by prefixing names with `@`.
          For instance, `@wheel` means all users in the `wheel` group.

          > **Warning**
          >
          > Adding a user to `trusted-users` is essentially equivalent to giving that user root access to the system.
          > For example, the user can access or replace store path contents that are critical for system security.
        )"};

  setting_t<strings_t> allowed_users{this,
                                     {"*"},
                                     "allowed-users",
                                     R"(
          A list of user names, separated by whitespace.
          These users are allowed to connect to the Nix daemon.

          You can specify groups by prefixing names with `@`.
          For instance, `@wheel` means all users in the `wheel` group.
          Also, you can allow all users by specifying `*`.

          > **Note**
          >
          > Trusted users (set in [`trusted-users`](#conf-trusted-users)) can always connect to the Nix daemon.
        )"};
};

extern authorization_settings_t authorization_settings;

#ifndef _WIN32
/**
 * Check if the given user/group matches the given user/group whitelist.
 *
 * @param user The username (or numeric UID as string if username unavailable)
 * @param group The primary group name (or numeric GID as string if unavailable)
 * @param users The whitelist of users/groups to match against
 * @return true if the user matches, false otherwise
 *
 * Matching rules:
 * - If the list contains "*", all users match
 * - If the username is in the list, match
 * - If an entry starts with "@", it's a group name; match if user is in that group
 */
bool match_user(const std::optional<std::string>& user, const std::optional<std::string>& group,
                const strings_t& users);
#endif

} // namespace nix
