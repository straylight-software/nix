#pragma once
///@file

#include <future>
#include <thread>

#include "nix/store/build/goal.h"
#include "nix/store/realisation.h"
#include "nix/store/store-api.h"
#include "nix/util/muxable-pipe.h"

namespace nix {

class Worker;

/**
 * Substitution of a derivation output.
 * This is done in three steps:
 * 1. Fetch the output info from a substituter
 * 2. Substitute the corresponding output path
 * 3. Register the output info
 */
class DrvOutputSubstitutionGoal : public Goal {
  /**
   * The drv output we're trying to substitute
   */
  DrvOutput id;

public:
  DrvOutputSubstitutionGoal(const DrvOutput& id, Worker& worker);

  Co init();

  void timedOut(Error&& ex) override { unreachable(); };

  std::string key() override;

  void handleEOF(descriptor_t fd) override;

  JobCategory jobCategory() const override { return JobCategory::Substitution; };
};

} // namespace nix
