// straylight::nix::build::reapi_build_service
//
// REAPI (Remote Execution API) build service implementation.
// Delegates builds to a gRPC REAPI server like nativelink.
//
// STATUS: Stub - not yet implemented.
// TODO: Implement when nativelink integration is ready.

#include "build_service.h"
#include "nix/util/error.h"

namespace straylight::nix::build {

// Stub factory - returns nullptr until REAPI is implemented
std::unique_ptr<build_service> make_reapi_build_service(const std::string& /* endpoint */,
                                                        const std::string& /* instance_name */) {
  // Not yet implemented - return nullptr
  // Callers should check for nullptr and fall back to daemon service
  return nullptr;
}

} // namespace straylight::nix::build
