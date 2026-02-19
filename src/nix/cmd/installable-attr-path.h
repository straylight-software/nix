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
  std::string attrPath;
  ExtendedOutputsSpec extendedOutputsSpec;

  InstallableAttrPath(ref<EvalState> state, SourceExprCommand& cmd, Value* v,
                      const std::string& attrPath, ExtendedOutputsSpec extendedOutputsSpec);

  std::string what() const override { return attrPath; };

  std::pair<Value*, PosIdx> toValue(EvalState& state) override;

  DerivedPathsWithInfo toDerivedPaths() override;

public:
  static InstallableAttrPath parse(ref<EvalState> state, SourceExprCommand& cmd, Value* v,
                                   std::string_view prefix,
                                   ExtendedOutputsSpec extendedOutputsSpec);
};

} // namespace nix
