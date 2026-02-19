#pragma once
///@file

#include "nix/store/store-api.h"
#include "nix/util/serialise.h"

namespace nix::daemon {

enum RecursiveFlag : bool { NotRecursive = false, Recursive = true };

void processConnection(ref<Store> store, FdSource&& from, FdSink&& to, TrustedFlag trusted,
                       RecursiveFlag recursive);

} // namespace nix::daemon
