#pragma once
///@file

#include "nix/store/store-api.h"
#include "nix/util/serialise.h"

namespace nix::daemon {

enum RecursiveFlag : bool { NotRecursive = false, Recursive = true };

void processConnection(ref<Store> store, fd_source_t&& from, fd_sink_t&& to, TrustedFlag trusted,
                       RecursiveFlag recursive);

} // namespace nix::daemon
