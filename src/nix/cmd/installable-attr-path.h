#pragma once
///@file

#include <queue>
#include <regex>

#include <nlohmann/json.hpp>

#include "nix/cmd/command.h"
#include "nix/cmd/common-eval-args.h"
#include "nix/cmd/installable-value.h"
#include "nix/expr/attr-path.h"
#include "nix/expr/eval-cache.h"
#include "nix/expr/eval-inline.h"
#include "nix/expr/eval.h"
#include "nix/expr/get-drvs.h"
#include "nix/fetchers/registry.h"
#include "nix/main/shared.h"
#include "nix/store/build-result.h"
#include "nix/store/derivations.h"
#include "nix/store/globals.h"
#include "nix/store/outputs-spec.h"
#include "nix/store/store-api.h"
#include "nix/util/url.h"

namespace nix {

class InstallableAttrPath : public InstallableValue {
  SourceExprCommand& cmd;
  RootValue v;
  std::string attr_path;
  ExtendedOutputsSpec extendedOutputsSpec;

  InstallableAttrPath(ref<eval_state_t> state, SourceExprCommand& cmd, value_t* v,
                      const std::string& attr_path, ExtendedOutputsSpec extendedOutputsSpec);

  std::string what() const override { return attr_path; };

  std::pair<value_t*, pos_idx_t> toValue(eval_state_t& state) override;

  DerivedPathsWithInfo to_derived_paths() override;

public:
  static InstallableAttrPath parse(ref<eval_state_t> state, SourceExprCommand& cmd, value_t* v,
                                   std::string_view prefix,
                                   ExtendedOutputsSpec extendedOutputsSpec);
};

} // namespace nix
