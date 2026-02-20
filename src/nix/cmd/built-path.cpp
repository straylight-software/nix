#include "nix/cmd/built-path.h"

#include <optional>

#include <nlohmann/json.hpp>

#include "nix/store/derivations.h"
#include "nix/store/store-api.h"
#include "nix/util/comparator.h"

namespace nix {

// Custom implementation to avoid `ref` ptr equality
GENERATE_CMP_EXT(, std::strong_ordering, SingleBuiltPathBuilt, *me->drv_path, me->output);

// Custom implementation to avoid `ref` ptr equality

// TODO no `GENERATE_CMP_EXT` because no `std::set::operator<=>` on
// Darwin, per header.
GENERATE_EQUAL(, BuiltPathBuilt ::, BuiltPathBuilt, *me->drv_path, me->outputs);

store_path_t SingleBuiltPath::out_path() const {
  return std::visit(overloaded{
                        [](const SingleBuiltPath::opaque_t& p) { return p.path; },
                        [](const SingleBuiltPath::Built& b) { return b.output.second; },
                    },
                    raw());
}

store_path_set_t BuiltPath::out_paths() const {
  return std::visit(overloaded{
                        [](const BuiltPath::opaque_t& p) { return store_path_set_t{p.path}; },
                        [](const BuiltPath::Built& b) {
                          store_path_set_t res;
                          for (auto& [_, path] : b.outputs)
                            res.insert(path);
                          return res;
                        },
                    },
                    raw());
}

SingleDerivedPath::Built SingleBuiltPath::Built::discardOutputPath() const {
  return SingleDerivedPath::Built{
      .drv_path = make_ref<SingleDerivedPath>(drv_path->discardOutputPath()),
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

nlohmann::json BuiltPath::Built::to_json(const store_dir_config_t& store) const {
  nlohmann::json res;
  res["drvPath"] = drv_path->to_json(store);
  for (const auto& [output_name, output_path] : outputs) {
    res["outputs"][output_name] = store.printStorePath(output_path);
  }
  return res;
}

nlohmann::json SingleBuiltPath::Built::to_json(const store_dir_config_t& store) const {
  nlohmann::json res;
  res["drvPath"] = drv_path->to_json(store);
  auto& [output_name, output_path] = output;
  res["output"] = output_name;
  res["outputPath"] = store.printStorePath(output_path);
  return res;
}

nlohmann::json SingleBuiltPath::to_json(const store_dir_config_t& store) const {
  return std::visit(overloaded{
                        [&](const SingleBuiltPath::opaque_t& o) -> nlohmann::json {
                          return store.printStorePath(o.path);
                        },
                        [&](const SingleBuiltPath::Built& b) { return b.to_json(store); },
                    },
                    raw());
}

nlohmann::json BuiltPath::to_json(const store_dir_config_t& store) const {
  return std::visit(overloaded{
                        [&](const BuiltPath::opaque_t& o) -> nlohmann::json {
                          return store.printStorePath(o.path);
                        },
                        [&](const BuiltPath::Built& b) { return b.to_json(store); },
                    },
                    raw());
}

RealisedPath::Set BuiltPath::toRealisedPaths(store_t& store) const {
  RealisedPath::Set res;
  std::visit(overloaded{
                 [&](const BuiltPath::opaque_t& p) { res.insert(p.path); },
                 [&](const BuiltPath::Built& p) {
                   auto drv_hashes =
                       static_output_hashes(store, store.read_derivation(p.drv_path->out_path()));
                   for (auto& [output_name, output_path] : p.outputs) {
                     if (experimental_feature_settings.is_enabled(xp_t::ca_derivations)) {
                       auto drvOutput = get(drv_hashes, output_name);
                       if (!drvOutput)
                         throw Error("the derivation '%s' has unrealised output '%s' "
                                     "(derived-path.cc/toRealisedPaths)",
                                     store.printStorePath(p.drv_path->out_path()), output_name);
                       DrvOutput key{*drvOutput, output_name};
                       auto thisRealisation = store.query_realisation(key);
                       assert(thisRealisation); // We’ve built it, so we must
                                                // have the realisation
                       res.insert(realisation_t{*thisRealisation, std::move(key)});
                     } else {
                       res.insert(output_path);
                     }
                   }
                 },
             },
             raw());
  return res;
}

} // namespace nix
