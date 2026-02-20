#pragma once
/**
 * @file Misc type definitions for both local building and remote (RPC building)
 */

#include "nix/store/path.h"
#include "nix/util/hash.h"

namespace nix {

class store_t;
struct derivation_t;
struct store_dir_config_t;

/**
 * Unless we are repairing, we don't both to test validity and just assume it,
 * so the choices are `Absent` or `Valid`.
 */
enum struct PathStatus {
  Corrupt,
  Absent,
  Valid,
};

struct InitialOutputStatus {
  store_path_t path;
  PathStatus status;

  /**
   * Valid in the store, and additionally non-corrupt if we are repairing
   */
  bool isValid() const { return status == PathStatus::Valid; }

  /**
   * Merely present, allowed to be corrupt
   */
  bool isPresent() const { return status == PathStatus::Corrupt || status == PathStatus::Valid; }
};

struct InitialOutput {
  Hash outputHash;
  std::optional<InitialOutputStatus> known;
};

/**
 * Format the known outputs of a derivation for use in error messages.
 */
std::string show_known_outputs(const store_dir_config_t& store, const derivation_t& drv);

} // namespace nix
