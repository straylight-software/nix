#include <sstream>

#include "nix/cmd/command.h"
#include "nix/main/shared.h"
#include "nix/store/globals.h"
#include "nix/store/local-fs-store.h"
#include "nix/store/serve-protocol.h"
#include "nix/store/store-api.h"
#include "nix/store/worker-protocol.h"
#include "nix/util/executable-path.h"
#include "nix/util/exit.h"
#include "nix/util/logging.h"

namespace nix::fs {
using namespace std::filesystem;
}

using namespace nix;

namespace {

std::string format_protocol(unsigned int proto) {
  if (proto) {
    auto major = GET_PROTOCOL_MAJOR(proto) >> 8;
    auto minor = GET_PROTOCOL_MINOR(proto);
    return fmt("%1%.%2%", major, minor);
  }
  return "unknown";
}

bool check_pass(std::string_view msg) {
  notice(ANSI_GREEN "[PASS] " ANSI_NORMAL + msg);
  return true;
}

bool check_fail(std::string_view msg) {
  notice(ANSI_RED "[FAIL] " ANSI_NORMAL + msg);
  return false;
}

void check_info(std::string_view msg) {
  notice(ANSI_BLUE "[INFO] " ANSI_NORMAL + msg);
}

} // namespace

struct cmd_config_check_t : StoreCommand {
  bool success = true;

  /**
   * This command is stable before the others
   */
  std::optional<experimental_feature_t> experimental_feature() override { return std::nullopt; }

  std::string description() override {
    return "check your system for potential problems and print a PASS or FAIL for each check";
  }

  category_t category() override { return catNixInstallation; }

  void run(ref<store_t> store) override {
    logger->log("Running checks against store uri: " + store->config.getHumanReadableURI());

    if (store.dynamic_pointer_cast<local_fs_store>()) {
      success &= check_nix_in_path();
      success &= check_profile_roots(store);
    }
    success &= check_store_protocol(store->getProtocol());
    check_trusted_user(store);

    if (!success)
      throw exit_t(2);
  }

  bool check_nix_in_path() {
    std::set<std::filesystem::path> dirs;

    for (auto& dir : executable_path_t::load().directories) {
      auto candidate = dir / "nix-env";
      if (std::filesystem::exists(candidate))
        dirs.insert(std::filesystem::canonical(candidate).parent_path());
    }

    if (dirs.size() != 1) {
      std::ostringstream ss;
      ss << "Multiple versions of nix found in PATH:\n";
      for (auto& dir : dirs)
        ss << "  " << dir << "\n";
      return check_fail(ss.view());
    }

    return check_pass("PATH contains only one nix version.");
  }

  bool check_profile_roots(ref<store_t> store) {
    std::set<std::filesystem::path> dirs;

    for (auto& dir : executable_path_t::load().directories) {
      auto profileDir = dir.parent_path();
      try {
        auto user_env = std::filesystem::weakly_canonical(profileDir);

        auto noContainsProfiles = [&] {
          for (auto&& part : profileDir)
            if (part == "profiles")
              return false;
          return true;
        };

        if (store->isStorePath(user_env.string()) &&
            has_suffix(user_env.string(), "user-environment")) {
          while (noContainsProfiles() && std::filesystem::is_symlink(profileDir))
            profileDir = std::filesystem::weakly_canonical(
                profileDir.parent_path() / std::filesystem::read_symlink(profileDir));

          if (noContainsProfiles())
            dirs.insert(dir);
        }
      } catch (SystemError&) {
      } catch (std::filesystem::filesystem_error&) {
      }
    }

    if (!dirs.empty()) {
      std::ostringstream ss;
      ss << "Found profiles outside of " << settings.nixStateDir << "/profiles.\n"
         << "The generation this profile points to might not have a gcroot and could be\n"
         << "garbage collected, resulting in broken symlinks.\n\n";
      for (auto& dir : dirs)
        ss << "  " << dir << "\n";
      ss << "\n";
      return check_fail(ss.view());
    }

    return check_pass("All profiles are gcroots.");
  }

  bool check_store_protocol(unsigned int store_proto) {
    unsigned int client_proto =
        GET_PROTOCOL_MAJOR(SERVE_PROTOCOL_VERSION) == GET_PROTOCOL_MAJOR(store_proto)
            ? SERVE_PROTOCOL_VERSION
            : PROTOCOL_VERSION;

    if (client_proto != store_proto) {
      std::ostringstream ss;
      ss << "Warning: protocol version of this client does not match the store.\n"
         << "While this is not necessarily a problem it's recommended to keep the client in\n"
         << "sync with the daemon.\n\n"
         << "Client protocol: " << format_protocol(client_proto) << "\n"
         << "store_t protocol: " << format_protocol(store_proto) << "\n\n";
      return check_fail(ss.view());
    }

    return check_pass("Client protocol matches store protocol.");
  }

  void check_trusted_user(ref<store_t> store) {
    if (auto trustedMay = store->isTrustedClient()) {
      std::string_view trusted = trustedMay.value() ? "trusted" : "not trusted";
      check_info(fmt("You are %s by store uri: %s", trusted, store->config.getHumanReadableURI()));
    } else {
      check_info(fmt("store_t uri: %s doesn't have a notion of trusted user",
                     store->config.getHumanReadableURI()));
    }
  }
};

static auto r_cmd_config_check = registerCommand2<cmd_config_check_t>({"config", "check"});
