#include <nlohmann/json.hpp>

#include "nix/cmd/command.h"
#include "nix/main/common-args.h"
#include "nix/main/shared.h"
#include "nix/store/local-fs-store.h"
#include "nix/store/store-api.h"

using namespace nix;

/* This serialization code is diferent from the canonical (single)
   derived path serialization because:

   - It looks up output paths where possible

   - It includes the store dir in store paths

   We might want to replace it with the canonical format at some point,
   but that would be a breaking change (to a still-experimental but
   widely-used command, so that isn't being done at this time just yet.
 */

static nlohmann::json to_json(store_t& store, const SingleDerivedPath::opaque_t& o) {
  return store.printStorePath(o.path);
}

static nlohmann::json to_json(store_t& store, const SingleDerivedPath& sdp);
static nlohmann::json to_json(store_t& store, const derived_path_t& dp);

static nlohmann::json to_json(store_t& store, const SingleDerivedPath::Built& sdpb) {
  nlohmann::json res;
  res["drvPath"] = to_json(store, *sdpb.drv_path);
  // Fallback for the input-addressed derivation case: We expect to always be
  // able to print the output paths, so let’s do it
  // FIXME try-resolve on drvPath
  const auto output_map =
      store.queryPartialDerivationOutputMap(resolve_derived_path(store, *sdpb.drv_path));
  res["output"] = sdpb.output;
  auto output_path_iter = output_map.find(sdpb.output);
  if (output_path_iter == output_map.end())
    res["outputPath"] = nullptr;
  else if (std::optional p = output_path_iter->second)
    res["outputPath"] = store.printStorePath(*p);
  else
    res["outputPath"] = nullptr;
  return res;
}

static nlohmann::json to_json(store_t& store, const derived_path_t::Built& dpb) {
  nlohmann::json res;
  res["drvPath"] = to_json(store, *dpb.drv_path);
  // Fallback for the input-addressed derivation case: We expect to always be
  // able to print the output paths, so let’s do it
  // FIXME try-resolve on drvPath
  const auto output_map =
      store.queryPartialDerivationOutputMap(resolve_derived_path(store, *dpb.drv_path));
  for (const auto& [output, outputPathOpt] : output_map) {
    if (!dpb.outputs.contains(output))
      continue;
    if (outputPathOpt)
      res["outputs"][output] = store.printStorePath(*outputPathOpt);
    else
      res["outputs"][output] = nullptr;
  }
  return res;
}

static nlohmann::json to_json(store_t& store, const SingleDerivedPath& sdp) {
  return std::visit([&](const auto& buildable) { return to_json(store, buildable); }, sdp.raw());
}

static nlohmann::json to_json(store_t& store, const derived_path_t& dp) {
  return std::visit([&](const auto& buildable) { return to_json(store, buildable); }, dp.raw());
}

static nlohmann::json derived_paths_to_json(const DerivedPaths& paths, store_t& store) {
  auto res = nlohmann::json::array();
  for (auto& t : paths) {
    res.push_back(to_json(store, t));
  }
  return res;
}

static nlohmann::json built_paths_with_result_to_json(const std::vector<BuiltPathWithResult>& buildables,
                                                 const store_t& store) {
  auto res = nlohmann::json::array();
  for (auto& b : buildables) {
    auto j = b.path.to_json(store);
    if (b.result) {
      if (b.result->start_time)
        j["startTime"] = b.result->start_time;
      if (b.result->stopTime)
        j["stopTime"] = b.result->stopTime;
      if (b.result->cpu_user)
        j["cpuUser"] = ((double)b.result->cpu_user->count()) / 1000000;
      if (b.result->cpu_system)
        j["cpuSystem"] = ((double)b.result->cpu_system->count()) / 1000000;
    }
    res.push_back(j);
  }
  return res;
}

struct cmd_build_t : InstallablesCommand, MixOutLinkByDefault, MixDryRun, MixJSON, MixProfile {
  bool print_output_paths = false;
  BuildMode build_mode = bmNormal;

  cmd_build_t() {
    add_flag({
        .long_name = "print-out-paths",
        .description = "Print the resulting output paths",
        .handler = {&print_output_paths, true},
    });

    add_flag({
        .long_name = "rebuild",
        .description =
            "Rebuild an already built package and compare the result to the existing store paths.",
        .handler = {&build_mode, bmCheck},
    });
  }

  std::string description() override { return "build a derivation or fetch a store path"; }

  std::string doc() override {
    return
#include "build.md"
        ;
  }

  void run(ref<store_t> store, Installables&& installables) override {
    if (dry_run) {
      std::vector<derived_path_t> pathsToBuild;

      for (auto& i : installables)
        for (auto& b : i->to_derived_paths())
          pathsToBuild.push_back(b.path);

      print_missing(store, pathsToBuild, lvl_error);

      if (json)
        printJSON(derived_paths_to_json(pathsToBuild, *store));

      return;
    }

    auto buildables = Installable::build(getEvalStore(), store, Realise::Outputs, installables,
                                         repair ? bmRepair : build_mode);

    if (json)
      logger->cout("%s", built_paths_with_result_to_json(buildables, *store).dump());

    createOutLinksMaybe(buildables, store);

    if (print_output_paths) {
      logger->stop();
      for (auto& buildable : buildables) {
        std::visit(
            overloaded{
                [&](const BuiltPath::opaque_t& bo) { logger->cout(store->printStorePath(bo.path)); },
                [&](const BuiltPath::Built& bfd) {
                  for (auto& output : bfd.outputs) {
                    logger->cout(store->printStorePath(output.second));
                  }
                },
            },
            buildable.path.raw());
      }
    }

    BuiltPaths buildables2;
    for (auto& b : buildables)
      buildables2.push_back(b.path);
    updateProfile(buildables2);
  }
};

static auto r_cmd_build = registerCommand<cmd_build_t>("build");
