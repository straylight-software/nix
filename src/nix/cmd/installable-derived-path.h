#pragma once
///@file

#include "nix/cmd/installables.h"

namespace nix {

struct InstallableDerivedPath : Installable {
  ref<Store> store;
  DerivedPath derived_path;

  InstallableDerivedPath(ref<Store> store, DerivedPath&& derived_path)
      : store(store), derived_path(std::move(derived_path)) {}

  std::string what() const override;

  DerivedPathsWithInfo to_derived_paths() override;

  std::optional<StorePath> getStorePath() override;

  static InstallableDerivedPath parse(ref<Store> store, std::string_view prefix,
                                      ExtendedOutputsSpec extendedOutputsSpec);
};

} // namespace nix
