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

// Required for notice/printInfo/printError macros
using nix::fmt;
using nix::logger;

namespace {

std::string format_protocol(unsigned int proto) {
  if (proto) {
    auto major = GET_PROTOCOL_MAJOR(proto) >> 8;
    auto minor = GET_PROTOCOL_MINOR(proto);
    return nix::fmt("%1%.%2%", major, minor);
  }
  return "unknown";
}

bool check_pass(std::string_view msg) {
  notice("%s", (ANSI_GREEN "[PASS] " ANSI_NORMAL + std::string(msg)).c_str());
  return true;
}

bool check_fail(std::string_view msg) {
  notice("%s", (ANSI_RED "[FAIL] " ANSI_NORMAL + std::string(msg)).c_str());
  return false;
}

void check_info(std::string_view msg) {
  notice("%s", (ANSI_BLUE "[INFO] " ANSI_NORMAL + std::string(msg)).c_str());
}

} // namespace

struct cmd_config_check_t : nix::StoreCommand {
  bool success = true;

  /**
   * This command is stable before the others
   */
  std::optional<nix::experimental_feature_t> experimental_feature() override {
    return std::nullopt;
  }

  std::string description() override {
    return "check your system for potential problems and print a PASS or FAIL for each check";
  }

  category_t category() override { return nix::catNixInstallation; }

  void run(nix::ref<nix::store_t> store) override {
    nix::logger->log("Running checks against store uri: " + store->config.getHumanReadableURI());

    if (store.dynamic_pointer_cast<nix::local_fs_store>()) {
      success &= check_nix_in_path();
      success &= check_profile_roots(store);
    }
    success &= check_store_protocol(store->getProtocol());
    check_trusted_user(store);

    if (!success)
      throw nix::exit_t(2);
  }

  bool check_nix_in_path() {
    std::set<std::filesystem::path> dirs;

    for (auto& dir : nix::executable_path_t::load().directories) {
      auto candidate = dir / "nix-env";
      if (std::filesystem::exists(candidate))
        dirs.insert(std::filesystem::canonical(candidate).parent_path());
    }

    if (dirs.size() != 1) {
      std::string msg = "Multiple versions of nix found in PATH:\n";
      for (auto& dir : dirs)
        msg += "  " + dir.string() + "\n";
      return check_fail(msg);
    }

    return check_pass("PATH contains only one nix version.");
  }

  bool check_profile_roots(nix::ref<nix::store_t> store) {
    std::set<std::filesystem::path> dirs;

    for (auto& dir : nix::executable_path_t::load().directories) {
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
            nix::has_suffix(user_env.string(), "user-environment")) {
          while (noContainsProfiles() && std::filesystem::is_symlink(profileDir))
            profileDir = std::filesystem::weakly_canonical(
                profileDir.parent_path() / std::filesystem::read_symlink(profileDir));

          if (noContainsProfiles())
            dirs.insert(dir);
        }
      } catch (nix::SystemError&) {
      } catch (std::filesystem::filesystem_error&) {
      }
    }

    if (!dirs.empty()) {
      std::string msg =
          "Found profiles outside of " + nix::settings.nixStateDir +
          "/profiles.\n"
          "The generation this profile points to might not have a gcroot and could be\n"
          "garbage collected, resulting in broken symlinks.\n\n";
      for (auto& dir : dirs)
        msg += "  " + dir.string() + "\n";
      msg += "\n";
      return check_fail(msg);
    }

    return check_pass("All profiles are gcroots.");
  }

  bool check_store_protocol(unsigned int store_proto) {
    unsigned int client_proto =
        GET_PROTOCOL_MAJOR(SERVE_PROTOCOL_VERSION) == GET_PROTOCOL_MAJOR(store_proto)
            ? SERVE_PROTOCOL_VERSION
            : PROTOCOL_VERSION;

    if (client_proto != store_proto) {
      std::string msg =
          "Warning: protocol version of this client does not match the store.\n"
          "While this is not necessarily a problem it's recommended to keep the client in\n"
          "sync with the daemon.\n\n"
          "Client protocol: " +
          format_protocol(client_proto) +
          "\n"
          "store_t protocol: " +
          format_protocol(store_proto) + "\n\n";
      return check_fail(msg);
    }

    return check_pass("Client protocol matches store protocol.");
  }

  void check_trusted_user(nix::ref<nix::store_t> store) {
    if (auto trustedMay = store->isTrustedClient()) {
      std::string_view trusted = trustedMay.value() ? "trusted" : "not trusted";
      check_info(
          nix::fmt("You are %s by store uri: %s", trusted, store->config.getHumanReadableURI()));
    } else {
      check_info(nix::fmt("store_t uri: %s doesn't have a notion of trusted user",
                          store->config.getHumanReadableURI()));
    }
  }
};

static auto r_cmd_config_check = nix::registerCommand2<cmd_config_check_t>({"config", "check"});
