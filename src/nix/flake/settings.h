#pragma once
///@file

#include <string>

#include <sys/types.h>

#include "nix/util/configuration.h"

namespace nix {
// Forward declarations
struct EvalSettings;

} // namespace nix

namespace nix::flake {

struct settings_t : public Config {
  settings_t();

  void configureEvalSettings(nix::EvalSettings& evalSettings) const;

  setting_t<bool> useRegistries{
      this, true, "use-registries", "Whether to use flake registries to resolve flake references.",
      {},   true};

  setting_t<bool> acceptFlakeConfig{
      this,
      false,
      "accept-flake-config",
      "Whether to accept Nix configuration settings from a flake without prompting.",
      {},
      true};

  setting_t<std::string> commitLockFileSummary{this,
                                             "",
                                             "commit-lock-file-summary",
                                             R"(
          The commit summary to use when committing changed flake lock files. If
          empty, the summary is generated based on the action performed.
        )",
                                             {"commit-lockfile-summary"},
                                             true};
};

} // namespace nix::flake
