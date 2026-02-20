#pragma once
///@file

#include "nix/cmd/installables.h"

namespace nix {

struct InstallableDerivedPath : Installable {
  ref<store_t> store;
  derived_path_t derived_path;

  InstallableDerivedPath(ref<store_t> store, derived_path_t&& derived_path)
      : store(store), derived_path(std::move(derived_path)) {}

  std::string what() const override;

  DerivedPathsWithInfo to_derived_paths() override;

  std::optional<store_path_t> getStorePath() override;

  static InstallableDerivedPath parse(ref<store_t> store, std::string_view prefix,
                                      ExtendedOutputsSpec extendedOutputsSpec);
};

} // namespace nix
