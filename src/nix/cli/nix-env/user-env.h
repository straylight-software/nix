#pragma once
///@file

#include "nix/expr/get-drvs.h"

namespace nix {

PackageInfos query_installed(eval_state_t& state, const Path& user_env);

bool create_user_env(eval_state_t& state, PackageInfos& elems, const Path& profile, bool keep_derivations,
                   const std::string& lock_token);

} // namespace nix
