#include <boost/unordered/unordered_flat_set.hpp>

#include "nix/store/derivation-options.h"
#include "nix/store/derivations.h"
#include "nix/store/filetransfer.h"
#include "nix/store/globals.h"
#include "nix/store/parsed-derivations.h"
#include "nix/store/realisation.h"
#include "nix/store/store-open.h"
#include "nix/util/callback.h"
#include "nix/util/closure.h"
#include "nix/util/json-utils.h"
#include "nix/util/strings.h"
#include "nix/util/thread-pool.h"
#include "nix/util/topo-sort.h"

namespace nix {

void Store::computeFSClosure(const StorePathSet& startPaths, StorePathSet& paths_,
                             bool flipDirection, bool includeOutputs, bool includeDerivers) {
  std::function<std::set<StorePath>(const StorePath& path, std::future<ref<const ValidPathInfo>>&)>
      queryDeps;
  if (flipDirection)
    queryDeps = [&](const StorePath& path, std::future<ref<const ValidPathInfo>>& fut) {
      StorePathSet res;
      StorePathSet referrers;
      query_referrers(path, referrers);
      for (auto& ref : referrers)
        if (ref != path)
          res.insert(ref);

      if (includeOutputs)
        for (auto& i : queryValidDerivers(path))
          res.insert(i);

      if (includeDerivers && path.is_derivation())
        for (auto& [_, maybeOutPath] : queryPartialDerivationOutputMap(path))
          if (maybeOutPath && isValidPath(*maybeOutPath))
            res.insert(*maybeOutPath);
      return res;
    };
  else
    queryDeps = [&](const StorePath& path, std::future<ref<const ValidPathInfo>>& fut) {
      StorePathSet res;
      auto info = fut.get();
      for (auto& ref : info->references)
        if (ref != path)
          res.insert(ref);

      if (includeOutputs && path.is_derivation())
        for (auto& [_, maybeOutPath] : queryPartialDerivationOutputMap(path))
          if (maybeOutPath && isValidPath(*maybeOutPath))
            res.insert(*maybeOutPath);

      if (includeDerivers && info->deriver && isValidPath(*info->deriver))
        res.insert(*info->deriver);
      return res;
    };

  compute_closure<StorePath>(
      startPaths, paths_,
      [&](const StorePath& path,
          std::function<void(std::promise<std::set<StorePath>>&)> processEdges) {
        std::promise<std::set<StorePath>> promise;
        std::function<void(std::future<ref<const ValidPathInfo>>)> getDependencies =
            [&](std::future<ref<const ValidPathInfo>> fut) {
              try {
                promise.set_value(queryDeps(path, fut));
              } catch (...) {
                promise.set_exception(std::current_exception());
              }
            };
        queryPathInfo(path, getDependencies);
        processEdges(promise);
      });
}

void Store::computeFSClosure(const StorePath& startPath, StorePathSet& paths_, bool flipDirection,
                             bool includeOutputs, bool includeDerivers) {
  StorePathSet paths;
  paths.insert(startPath);
  computeFSClosure(paths, paths_, flipDirection, includeOutputs, includeDerivers);
}

const ContentAddress* get_derivation_ca(const BasicDerivation& drv) {
  auto out = drv.outputs.find("out");
  if (out == drv.outputs.end())
    return nullptr;
  if (auto dof = std::get_if<DerivationOutput::CAFixed>(&out->second.raw)) {
    return &dof->ca;
  }
  return nullptr;
}

MissingPaths Store::query_missing(const std::vector<DerivedPath>& targets) {
  activity_t act(*logger, lvl_debug, act_unknown, "querying info about missing paths");

  // FIXME: make async.
  thread_pool_t pool(file_transfer_settings.httpConnections);

  struct State {
    boost::unordered_flat_set<std::string> done;
    MissingPaths res;
  };

  struct DrvState {
    size_t left;
    bool done = false;
    StorePathSet out_paths;

    DrvState(size_t left) : left(left) {}
  };

  sync_t<State> state_;

  std::function<void(DerivedPath)> do_path;

  auto enqueueDerivedPaths =
      [&](this auto self, ref<SingleDerivedPath> input_drv,
          const DerivedPathMap<string_set_t>::ChildNode& input_node) -> void {
    if (!input_node.value.empty())
      pool.enqueue(std::bind(do_path, DerivedPath::Built{input_drv, input_node.value}));
    for (const auto& [output_name, childNode] : input_node.childMap)
      self(make_ref<SingleDerivedPath>(SingleDerivedPath::Built{input_drv, output_name}),
           childNode);
  };

  auto mustBuildDrv = [&](const StorePath& drv_path, const Derivation& drv) {
    {
      auto state(state_.lock());
      state->res.willBuild.insert(drv_path);
    }

    for (const auto& [input_drv, input_node] : drv.input_drvs.map) {
      enqueueDerivedPaths(makeConstantStorePathRef(input_drv), input_node);
    }
  };

  auto checkOutput = [&](const StorePath& drv_path, ref<Derivation> drv, const StorePath& out_path,
                         ref<sync_t<DrvState>> drvState_) {
    if (drvState_->lock()->done)
      return;

    SubstitutablePathInfos infos;
    auto* cap = get_derivation_ca(*drv);
    querySubstitutablePathInfos(
        {
            {
                out_path,
                cap ? std::optional{*cap} : std::nullopt,
            },
        },
        infos);

    if (infos.empty()) {
      drvState_->lock()->done = true;
      mustBuildDrv(drv_path, *drv);
    } else {
      {
        auto drvState(drvState_->lock());
        if (drvState->done)
          return;
        assert(drvState->left);
        drvState->left--;
        drvState->out_paths.insert(out_path);
        if (!drvState->left) {
          for (auto& path : drvState->out_paths)
            pool.enqueue(std::bind(do_path, DerivedPath::opaque_t{path}));
        }
      }
    }
  };

  do_path = [&](const DerivedPath& req) {
    {
      auto state(state_.lock());
      if (!state->done.insert(req.to_string(*this)).second)
        return;
    }

    std::visit(overloaded{
                   [&](const DerivedPath::Built& bfd) {
                     auto drvPathP = std::get_if<DerivedPath::opaque_t>(&*bfd.drv_path);
                     if (!drvPathP) {
                       // TODO make work in this case.
                       warn("Ignoring dynamic derivation %s while querying missing paths; not yet "
                            "implemented",
                            bfd.drv_path->to_string(*this));
                       return;
                     }
                     auto& drv_path = drvPathP->path;

                     if (!isValidPath(drv_path)) {
                       // FIXME: we could try to substitute the derivation.
                       auto state(state_.lock());
                       state->res.unknown.insert(drv_path);
                       return;
                     }

                     StorePathSet invalid;
                     /* true for regular derivations, and CA derivations for which we
                        have a trust mapping for all wanted outputs. */
                     auto knownOutputPaths = true;
                     for (auto& [output_name, pathOpt] :
                          queryPartialDerivationOutputMap(drv_path)) {
                       if (!pathOpt) {
                         knownOutputPaths = false;
                         break;
                       }
                       if (bfd.outputs.contains(output_name) && !isValidPath(*pathOpt))
                         invalid.insert(*pathOpt);
                     }
                     if (knownOutputPaths && invalid.empty())
                       return;

                     auto drv = make_ref<Derivation>(derivationFromPath(drv_path));
                     DerivationOptions<SingleDerivedPath> drv_options;
                     try {
                       // FIXME: this is a lot of work just to get the value
                       // of `allowSubstitutes`.
                       drv_options = derivation_options_from_structured_attrs(
                           *this, drv->input_drvs, drv->env, get(drv->structured_attrs));
                     } catch (Error& e) {
                       e.add_trace({}, "while parsing derivation '%s'", printStorePath(drv_path));
                       throw;
                     }

                     if (!knownOutputPaths && settings.use_substitutes &&
                         drv_options.substitutesAllowed()) {
                       experimental_feature_settings.require(xp_t::ca_derivations);

                       // If there are unknown output paths, attempt to find if the
                       // paths are known to substituters through a realisation.
                       auto output_hashes = static_output_hashes(*this, *drv);
                       knownOutputPaths = true;

                       for (auto [output_name, hash] : output_hashes) {
                         if (!bfd.outputs.contains(output_name))
                           continue;

                         bool found = false;
                         for (auto& sub : get_default_substituters()) {
                           auto realisation = sub->query_realisation({hash, output_name});
                           if (!realisation)
                             continue;
                           found = true;
                           if (!isValidPath(realisation->out_path))
                             invalid.insert(realisation->out_path);
                           break;
                         }
                         if (!found) {
                           // Some paths did not have a realisation, this must be built.
                           knownOutputPaths = false;
                           break;
                         }
                       }
                     }

                     if (knownOutputPaths && settings.use_substitutes &&
                         drv_options.substitutesAllowed()) {
                       auto drvState = make_ref<sync_t<DrvState>>(DrvState(invalid.size()));
                       for (auto& output : invalid)
                         pool.enqueue(std::bind(checkOutput, drv_path, drv, output, drvState));
                     } else
                       mustBuildDrv(drv_path, *drv);
                   },
                   [&](const DerivedPath::opaque_t& bo) {
                     if (isValidPath(bo.path))
                       return;

                     SubstitutablePathInfos infos;
                     querySubstitutablePathInfos({{bo.path, std::nullopt}}, infos);

                     if (infos.empty()) {
                       auto state(state_.lock());
                       state->res.unknown.insert(bo.path);
                       return;
                     }

                     auto info = infos.find(bo.path);
                     assert(info != infos.end());

                     {
                       auto state(state_.lock());
                       state->res.willSubstitute.insert(bo.path);
                       state->res.downloadSize += info->second.downloadSize;
                       state->res.nar_size += info->second.nar_size;
                     }

                     for (auto& ref : info->second.references)
                       pool.enqueue(std::bind(do_path, DerivedPath::opaque_t{ref}));
                   },
               },
               req.raw());
  };

  for (auto& path : targets)
    pool.enqueue(std::bind(do_path, path));

  pool.process();

  return std::move(state_.lock()->res);
}

StorePaths Store::topoSortPaths(const StorePathSet& paths) {
  auto result = topoSort(paths, [&](const StorePath& path) {
    try {
      return queryPathInfo(path)->references;
    } catch (InvalidPath&) {
      return StorePathSet();
    }
  });

  return std::visit(overloaded{[&](const Cycle<StorePath>& cycle) -> StorePaths {
                                 throw BuildError(
                                     BuildResult::Failure::OutputRejected,
                                     "cycle detected in the references of '%s' from '%s'",
                                     printStorePath(cycle.path), printStorePath(cycle.parent));
                               },
                               [](const auto& sorted) { return sorted; }},
                    result);
}

std::map<DrvOutput, StorePath>
drv_output_references(const std::set<Realisation>& input_realisations,
                      const StorePathSet& path_references) {
  std::map<DrvOutput, StorePath> res;

  for (const auto& input : input_realisations) {
    if (path_references.count(input.out_path)) {
      res.insert({input.id, input.out_path});
    }
  }

  return res;
}

std::map<DrvOutput, StorePath> drv_output_references(Store& store, const Derivation& drv,
                                                     const StorePath& output_path,
                                                     Store* eval_store_) {
  auto& eval_store = eval_store_ ? *eval_store_ : store;

  std::set<Realisation> input_realisations;

  auto accum_realisations = [&](this auto& self, const StorePath& input_drv,
                                const DerivedPathMap<string_set_t>::ChildNode& input_node) -> void {
    if (!input_node.value.empty()) {
      auto output_hashes = static_output_hashes(eval_store, eval_store.read_derivation(input_drv));
      for (const auto& output_name : input_node.value) {
        auto outputHash = get(output_hashes, output_name);
        if (!outputHash)
          throw Error("output '%s' of derivation '%s' isn't realised", output_name,
                      store.printStorePath(input_drv));
        DrvOutput key{*outputHash, output_name};
        auto thisRealisation = store.query_realisation(key);
        if (!thisRealisation)
          throw Error("output '%s' of derivation '%s' isn’t built", output_name,
                      store.printStorePath(input_drv));
        input_realisations.insert({*thisRealisation, std::move(key)});
      }
    }
    if (!input_node.value.empty()) {
      auto d = makeConstantStorePathRef(input_drv);
      for (const auto& [output_name, childNode] : input_node.childMap) {
        SingleDerivedPath next = SingleDerivedPath::Built{d, output_name};
        self(
            // TODO deep resolutions for dynamic derivations, issue #8947, would go here.
            resolve_derived_path(store, next, eval_store_), childNode);
      }
    }
  };

  for (const auto& [input_drv, input_node] : drv.input_drvs.map)
    accum_realisations(input_drv, input_node);

  auto info = store.queryPathInfo(output_path);

  return drv_output_references(Realisation::closure(store, input_realisations), info->references);
}

OutputPathMap resolve_derived_path(Store& store, const DerivedPath::Built& bfd,
                                   Store* eval_store_) {
  auto drv_path = resolve_derived_path(store, *bfd.drv_path, eval_store_);

  auto outputs_opt_ = store.queryPartialDerivationOutputMap(drv_path, eval_store_);

  auto outputs_opt =
      std::visit(overloaded{
                     [&](const OutputsSpec::All&) {
                       // Keep all outputs
                       return std::move(outputs_opt_);
                     },
                     [&](const OutputsSpec::Names& names) {
                       // Get just those mentioned by name
                       std::map<std::string, std::optional<StorePath>> outputs_opt;
                       for (auto& output : names) {
                         auto* pOutputPathOpt = get(outputs_opt_, output);
                         if (!pOutputPathOpt)
                           throw Error("the derivation '%s' doesn't have an output named '%s'",
                                       bfd.drv_path->to_string(store), output);
                         outputs_opt.insert_or_assign(output, std::move(*pOutputPathOpt));
                       }
                       return outputs_opt;
                     },
                 },
                 bfd.outputs.raw);

  OutputPathMap outputs;
  for (auto& [output_name, outputPathOpt] : outputs_opt) {
    if (!outputPathOpt)
      throw MissingRealisation(bfd.drv_path->to_string(store), output_name);
    auto& output_path = *outputPathOpt;
    outputs.insert_or_assign(output_name, output_path);
  }
  return outputs;
}

StorePath resolve_derived_path(Store& store, const SingleDerivedPath& req, Store* eval_store_) {
  auto& eval_store = eval_store_ ? *eval_store_ : store;

  return std::visit(overloaded{
                        [&](const SingleDerivedPath::opaque_t& bo) { return bo.path; },
                        [&](const SingleDerivedPath::Built& bfd) {
                          auto drv_path = resolve_derived_path(store, *bfd.drv_path, eval_store_);
                          auto output_paths =
                              eval_store.queryPartialDerivationOutputMap(drv_path, eval_store_);
                          if (output_paths.count(bfd.output) == 0)
                            throw Error("derivation '%s' does not have an output named '%s'",
                                        store.printStorePath(drv_path), bfd.output);
                          auto& optPath = output_paths.at(bfd.output);
                          if (!optPath)
                            throw MissingRealisation(bfd.drv_path->to_string(store), bfd.output);
                          return *optPath;
                        },
                    },
                    req.raw());
}

OutputPathMap resolve_derived_path(Store& store, const DerivedPath::Built& bfd) {
  auto drv_path = resolve_derived_path(store, *bfd.drv_path);
  auto output_map = store.queryDerivationOutputMap(drv_path);
  auto outputs_left = std::visit(
      overloaded{
          [&](const OutputsSpec::All&) { return string_set_t{}; },
          [&](const OutputsSpec::Names& names) { return static_cast<string_set_t>(names); },
      },
      bfd.outputs.raw);
  for (auto iter = output_map.begin(); iter != output_map.end();) {
    auto& output_name = iter->first;
    if (bfd.outputs.contains(output_name)) {
      outputs_left.erase(output_name);
      ++iter;
    } else {
      iter = output_map.erase(iter);
    }
  }
  if (!outputs_left.empty())
    throw Error(
        "derivation '%s' does not have an outputs %s", store.printStorePath(drv_path),
        concat_strings_sep(", ", quote_strings(std::get<OutputsSpec::Names>(bfd.outputs.raw))));
  return output_map;
}

} // namespace nix

namespace nlohmann {

using namespace nix;

TrustedFlag adl_serializer<TrustedFlag>::from_json(const json& json) {
  return get_boolean(json) ? TrustedFlag::Trusted : TrustedFlag::NotTrusted;
}

void adl_serializer<TrustedFlag>::to_json(json& json, const TrustedFlag& trustedFlag) {
  json = static_cast<bool>(trustedFlag);
}

} // namespace nlohmann
