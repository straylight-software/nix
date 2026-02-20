#include "nix/util/users.h"

#include <pwd.h>
#include <sys/types.h>
#include <unistd.h>

#include "nix/util/environment-variables.h"
#include "nix/util/file-system.h"
#include "nix/util/util.h"

namespace nix {

std::string get_user_name() {
  auto pw = getpwuid(geteuid());
  std::string name = pw ? pw->pw_name : get_env("USER").value_or("");
  if (name.empty()) {
    throw Error("cannot figure out user name");
  }
  return name;
}

std::filesystem::path get_home_of(uid_t user_id) {
  std::vector<char> buf(16384);
  struct passwd pwbuf;
  struct passwd* pw;
  if (getpwuid_r(user_id, &pwbuf, buf.data(), buf.size(), &pw) != 0 || !pw || !pw->pw_dir ||
      !pw->pw_dir[0]) {
    throw Error("cannot determine user's home directory");
  }
  return pw->pw_dir;
}

std::filesystem::path get_home() {
  static std::filesystem::path home_dir = []() {
    std::optional<std::string> unowned_user_home_dir = {};
    auto home_dir = get_env("HOME");
    if (home_dir) {
      // Only use $HOME if doesn't exist or is owned by the current user.
      struct stat st;
      int result = stat(home_dir->c_str(), &st);
      if (result != 0) {
        if (errno != ENOENT) {
          warn("couldn't stat $HOME ('%s') for reason other than not existing ('%d'), falling back "
               "to the one defined in the 'passwd' file",
               *home_dir, errno);
          home_dir.reset();
        }
      } else if (st.st_uid != geteuid()) {
        unowned_user_home_dir.swap(home_dir);
      }
    }
    if (!home_dir) {
      home_dir = get_home_of(geteuid());
      if (unowned_user_home_dir.has_value() && unowned_user_home_dir != home_dir) {
        warn("$HOME ('%s') is not owned by you, falling back to the one defined in the 'passwd' "
             "file ('%s')",
             *unowned_user_home_dir, *home_dir);
      }
    }
    return *home_dir;
  }();
  return home_dir;
}

bool is_root_user() {
  return getuid() == 0;
}

} // namespace nix
