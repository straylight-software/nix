#pragma once
///@file

#include <filesystem>

#include "nix/expr/eval-settings.h"
#include "nix/expr/search-path.h"
#include "nix/main/common-args.h"
#include "nix/util/args.h"
#include "nix/util/canon-path.h"

namespace nix {

class Store;

namespace fetchers {
struct settings_t;
}

class EvalState;
struct CompatibilitySettings;
class Bindings;

namespace flake {
struct settings_t;
}

extern fetchers::settings_t fetchSettings;

/**
 * @todo Get rid of global settings variables
 */
extern EvalSettings evalSettings;

/**
 * @todo Get rid of global settings variables
 */
extern flake::settings_t flakeSettings;

/**
 * settings_t that control behaviors that have changed since Nix 2.3.
 */
extern CompatibilitySettings compatibilitySettings;

struct MixEvalArgs : virtual Args, virtual MixRepair {
  static constexpr auto category = "Common evaluation options";

  MixEvalArgs();

  Bindings* getAutoArgs(EvalState& state);

  LookupPath lookupPath;

  std::optional<std::string> evalStoreUrl;

private:
  struct AutoArgExpr {
    std::string expr;
  };

  struct AutoArgString {
    std::string s;
  };

  struct AutoArgFile {
    std::filesystem::path path;
  };

  struct AutoArgStdin {};

  using AutoArg = std::variant<AutoArgExpr, AutoArgString, AutoArgFile, AutoArgStdin>;

  std::map<std::string, AutoArg> autoArgs;
};

/**
 * @param baseDir Optional [base
 * directory](https://nix.dev/manual/nix/development/glossary#gloss-base-directory)
 */
source_path_t lookupFileArg(EvalState& state, std::string_view s,
                         const std::filesystem::path* baseDir = nullptr);

} // namespace nix
