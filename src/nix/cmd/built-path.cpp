#include "nix/cmd/built-path.h"

#include <optional>

#include <nlohmann/json.hpp>

#include "nix/store/derivations.h"
#include "nix/store/store-api.h"
#include "nix/util/comparator.h"

namespace nix {

// Custom implementation to avoid `ref` ptr equality
GENERATE_CMP_EXT(, std::strong_ordering, SingleBuiltPathBuilt, *me->drvPath, me->output);

// Custom implementation to avoid `ref` ptr equality

// TODO no `GENERATE_CMP_EXT` because no `std::set::operator<=>` on
// Darwin, per header.
GENERATE_EQUAL(, BuiltPathBuilt ::, BuiltPathBuilt, *me->drvPath, me->outputs);

StorePath SingleBuiltPath::outPath() const {
  return std::visit(overloaded{
                        [](const SingleBuiltPath::opaque_t& p) { return p.path; },
                        [](const SingleBuiltPath::Built& b) { return b.output.second; },
                    },
                    raw());
}

StorePathSet BuiltPath::outPaths() const {
  return std::visit(overloaded{
                        [](const BuiltPath::opaque_t& p) { return StorePathSet{p.path}; },
                        [](const BuiltPath::Built& b) {
                          StorePathSet res;
                          for (auto& [_, path] : b.outputs)
                            res.insert(path);
                          return res;
                        },
                    },
                    raw());
}

SingleDerivedPath::Built SingleBuiltPath::Built::discardOutputPath() const {
  return SingleDerivedPath::Built{
      .drvPath = make_ref<SingleDerivedPath>(drvPath->discardOutputPath()),
      .output = output.first,
  };
}

SingleDerivedPath SingleBuiltPath::discardOutputPath() const {
  return std::visit(overloaded{
                        [](const SingleBuiltPath::opaque_t& p) -> SingleDerivedPath { return p; },
                        [](const SingleBuiltPath::Built& b) -> SingleDerivedPath {
                          return b.discardOutputPath();
                        },
                    },
                    raw());
}

nlohmann::json BuiltPath::Built::toJSON(const StoreDirConfig& store) const {
  nlohmann::json res;
  res["drvPath"] = drvPath->toJSON(store);
  for (const auto& [outputName, outputPath] : outputs) {
    res["outputs"][outputName] = store.printStorePath(outputPath);
  }
  return res;
}

nlohmann::json SingleBuiltPath::Built::toJSON(const StoreDirConfig& store) const {
  nlohmann::json res;
  res["drvPath"] = drvPath->toJSON(store);
  auto& [outputName, outputPath] = output;
  res["output"] = outputName;
  res["outputPath"] = store.printStorePath(outputPath);
  return res;
}

nlohmann::json SingleBuiltPath::toJSON(const StoreDirConfig& store) const {
  return std::visit(overloaded{
                        [&](const SingleBuiltPath::opaque_t& o) -> nlohmann::json {
                          return store.printStorePath(o.path);
                        },
                        [&](const SingleBuiltPath::Built& b) { return b.toJSON(store); },
                    },
                    raw());
}

nlohmann::json BuiltPath::toJSON(const StoreDirConfig& store) const {
  return std::visit(overloaded{
                        [&](const BuiltPath::opaque_t& o) -> nlohmann::json {
                          return store.printStorePath(o.path);
                        },
                        [&](const BuiltPath::Built& b) { return b.toJSON(store); },
                    },
                    raw());
}

RealisedPath::Set BuiltPath::toRealisedPaths(Store& store) const {
  RealisedPath::Set res;
  std::visit(overloaded{
                 [&](const BuiltPath::opaque_t& p) { res.insert(p.path); },
                 [&](const BuiltPath::Built& p) {
                   auto drvHashes =
                       staticOutputHashes(store, store.readDerivation(p.drvPath->outPath()));
                   for (auto& [outputName, outputPath] : p.outputs) {
                     if (experimentalFeatureSettings.isEnabled(xp_t::CaDerivations)) {
                       auto drvOutput = get(drvHashes, outputName);
                       if (!drvOutput)
                         throw Error("the derivation '%s' has unrealised output '%s' "
                                     "(derived-path.cc/toRealisedPaths)",
                                     store.printStorePath(p.drvPath->outPath()), outputName);
                       DrvOutput key{*drvOutput, outputName};
                       auto thisRealisation = store.queryRealisation(key);
                       assert(thisRealisation); // We’ve built it, so we must
                                                // have the realisation
                       res.insert(Realisation{*thisRealisation, std::move(key)});
                     } else {
                       res.insert(outputPath);
                     }
                   }
                 },
             },
             raw());
  return res;
}

} // namespace nix
