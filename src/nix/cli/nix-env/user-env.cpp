#include "user-env.h"

#include "nix/expr/eval.h"
#include "nix/expr/get-drvs.h"
#include "nix/util/file-system.h"

namespace nix {

PackageInfos query_installed(eval_state_t& state, const Path& user_env) {
  PackageInfos elems;
  auto manifest_json = std::filesystem::path(user_env) / "manifest.json";
  if (path_exists(manifest_json))
    throw Error("profile '%s' is incompatible with 'nix-env'; please use 'nix profile' instead",
                user_env);
  auto manifest_file = std::filesystem::path(user_env) / "manifest.nix";
  if (path_exists(manifest_file)) {
    value_t v;
    state.evalFile(state.root_path(canon_path_t(manifest_file.string())).resolve_symlinks(), v);
    bindings_t& bindings = bindings_t::emptyBindings;
    get_derivations(state, v, "", bindings, elems, false);
  }
  return elems;
}

bool create_user_env(eval_state_t& /* state */, PackageInfos& /* elems */,
                     const Path& /* profile */, bool /* keep_derivations */,
                     const std::string& /* lock_token */) {
  // Legacy nix-env user environment creation is not supported in this build.
  // Use 'nix profile' instead.
  throw Error("create_user_env is not implemented; use 'nix profile' instead");
}

} // namespace nix
