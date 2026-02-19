#include "nix/store/restricted-store.h"

#include "nix/store/build-result.h"
#include "nix/store/local-store.h"
#include "nix/store/realisation.h"
#include "nix/util/callback.h"

namespace nix {

static StorePath path_part_of_req(const SingleDerivedPath& req) {
  return std::visit(
      overloaded{
          [&](const SingleDerivedPath::opaque_t& bo) { return bo.path; },
          [&](const SingleDerivedPath::Built& bfd) { return path_part_of_req(*bfd.drv_path); },
      },
      req.raw());
}

static StorePath path_part_of_req(const DerivedPath& req) {
  return std::visit(overloaded{
                        [&](const DerivedPath::opaque_t& bo) { return bo.path; },
                        [&](const DerivedPath::Built& bfd) { return path_part_of_req(*bfd.drv_path); },
                    },
                    req.raw());
}

bool RestrictionContext::is_allowed(const DerivedPath& req) {
  return is_allowed(path_part_of_req(req));
}

/**
 * A wrapper around LocalStore that only allows building/querying of
 * paths that are in the input closures of the build or were added via
 * recursive Nix calls.
 */
struct restricted_store_t : public virtual IndirectRootStore, public virtual GcStore {
  ref<const LocalStore::config_t> config;

  ref<LocalStore> next;

  RestrictionContext& goal;

  restricted_store_t(ref<LocalStore::config_t> config, ref<LocalStore> next, RestrictionContext& goal)
      : Store{*config}, local_fs_store{*config}, config{config}, next(next), goal(goal) {}

  Path getRealStoreDir() override { return next->config->real_store_dir; }

  StorePathSet query_all_valid_paths() override;

  void
  query_path_info_uncached(const StorePath& path,
                        Callback<std::shared_ptr<const ValidPathInfo>> callback) noexcept override;

  void query_referrers(const StorePath& path, StorePathSet& referrers) override;

  std::map<std::string, std::optional<StorePath>>
  queryPartialDerivationOutputMap(const StorePath& path, Store* eval_store = nullptr) override;

  std::optional<StorePath> queryPathFromHashPart(const std::string& hash_part) override {
    throw Error("queryPathFromHashPart");
  }

  StorePath add_to_store(std::string_view name, const source_path_t& src_path,
                       ContentAddressMethod method, hash_algorithm_t hash_algo,
                       const StorePathSet& references, path_filter_t& filter,
                       RepairFlag repair) override {
    throw Error("addToStore");
  }

  void add_to_store(const ValidPathInfo& info, Source& nar_source, RepairFlag repair = NoRepair,
                  CheckSigsFlag check_sigs = CheckSigs) override;

  StorePath add_to_store_from_dump(Source& dump, std::string_view name,
                               file_serialisation_method_t dump_method, ContentAddressMethod hash_method,
                               hash_algorithm_t hash_algo, const StorePathSet& references,
                               RepairFlag repair) override;

  void nar_from_path(const StorePath& path, Sink& sink) override;

  void ensure_path(const StorePath& path) override;

  void register_drv_output(const Realisation& info) override;

  void query_realisation_uncached(
      const DrvOutput& id,
      Callback<std::shared_ptr<const UnkeyedRealisation>> callback) noexcept override;

  void build_paths(const std::vector<DerivedPath>& paths, BuildMode build_mode,
                  std::shared_ptr<Store> eval_store) override;

  std::vector<KeyedBuildResult>
  build_paths_with_results(const std::vector<DerivedPath>& paths, BuildMode build_mode = bmNormal,
                        std::shared_ptr<Store> eval_store = nullptr) override;

  BuildResult buildDerivation(const StorePath& drv_path, const BasicDerivation& drv,
                              BuildMode build_mode = bmNormal) override {
    unsupported("buildDerivation");
  }

  void addTempRoot(const StorePath& path) override {}

  void addIndirectRoot(const Path& path) override {}

  Roots findRoots(bool censor) override { return Roots(); }

  void collectGarbage(const GCOptions& options, GCResults& results) override {}

  void addSignatures(const StorePath& store_path, const string_set_t& sigs) override {
    unsupported("addSignatures");
  }

  MissingPaths query_missing(const std::vector<DerivedPath>& targets) override;

  virtual std::optional<std::string> getBuildLogExact(const StorePath& path) override {
    return std::nullopt;
  }

  virtual void addBuildLog(const StorePath& path, std::string_view log) override {
    unsupported("addBuildLog");
  }

  std::optional<TrustedFlag> isTrustedClient() override { return NotTrusted; }
};

ref<Store> make_restricted_store(ref<LocalStore::config_t> config, ref<LocalStore> next,
                               RestrictionContext& context) {
  return make_ref<restricted_store_t>(config, next, context);
}

StorePathSet restricted_store_t::query_all_valid_paths() {
  StorePathSet paths;
  for (auto& p : goal.originalPaths())
    paths.insert(p);
  for (auto& p : goal.addedPaths)
    paths.insert(p);
  return paths;
}

void restricted_store_t::query_path_info_uncached(
    const StorePath& path, Callback<std::shared_ptr<const ValidPathInfo>> callback) noexcept {
  if (goal.is_allowed(path)) {
    try {
      /* Censor impure information. */
      auto info = std::make_shared<ValidPathInfo>(*next->queryPathInfo(path));
      info->deriver.reset();
      info->registrationTime = 0;
      info->ultimate = false;
      info->sigs.clear();
      callback(info);
    } catch (InvalidPath&) {
      callback(nullptr);
    }
  } else
    callback(nullptr);
};

void restricted_store_t::query_referrers(const StorePath& path, StorePathSet& referrers) {}

std::map<std::string, std::optional<StorePath>>
restricted_store_t::queryPartialDerivationOutputMap(const StorePath& path, Store* eval_store) {
  if (!goal.is_allowed(path))
    throw InvalidPath("cannot query output map for unknown path '%s' in recursive Nix",
                      printStorePath(path));
  return next->queryPartialDerivationOutputMap(path, eval_store);
}

void restricted_store_t::add_to_store(const ValidPathInfo& info, Source& nar_source, RepairFlag repair,
                                 CheckSigsFlag check_sigs) {
  next->add_to_store(info, nar_source, repair, check_sigs);
  goal.addDependency(info.path);
}

StorePath restricted_store_t::add_to_store_from_dump(Source& dump, std::string_view name,
                                              file_serialisation_method_t dump_method,
                                              ContentAddressMethod hash_method,
                                              hash_algorithm_t hash_algo,
                                              const StorePathSet& references, RepairFlag repair) {
  auto path =
      next->add_to_store_from_dump(dump, name, dump_method, hash_method, hash_algo, references, repair);
  goal.addDependency(path);
  return path;
}

void restricted_store_t::nar_from_path(const StorePath& path, Sink& sink) {
  if (!goal.is_allowed(path))
    throw InvalidPath("cannot dump unknown path '%s' in recursive Nix", printStorePath(path));
  Store::nar_from_path(path, sink);
}

void restricted_store_t::ensure_path(const StorePath& path) {
  if (!goal.is_allowed(path))
    throw InvalidPath("cannot substitute unknown path '%s' in recursive Nix", printStorePath(path));
  /* Nothing to be done; 'path' must already be valid. */
}

void restricted_store_t::register_drv_output(const Realisation& info)
// XXX: This should probably be allowed as a no-op if the realisation
// corresponds to an allowed derivation
{
  throw Error("registerDrvOutput");
}

void restricted_store_t::query_realisation_uncached(
    const DrvOutput& id, Callback<std::shared_ptr<const UnkeyedRealisation>> callback) noexcept
// XXX: This should probably be allowed if the realisation corresponds to
// an allowed derivation
{
  if (!goal.is_allowed(id))
    callback(nullptr);
  next->query_realisation(id, std::move(callback));
}

void restricted_store_t::build_paths(const std::vector<DerivedPath>& paths, BuildMode build_mode,
                                 std::shared_ptr<Store> eval_store) {
  for (auto& result : build_paths_with_results(paths, build_mode, eval_store))
    if (auto* failureP = result.tryGetFailure())
      failureP->rethrow();
}

std::vector<KeyedBuildResult>
restricted_store_t::build_paths_with_results(const std::vector<DerivedPath>& paths, BuildMode build_mode,
                                       std::shared_ptr<Store> eval_store) {
  assert(!eval_store);

  if (build_mode != bmNormal)
    throw Error("unsupported build mode");

  StorePathSet new_paths;
  std::set<Realisation> newRealisations;

  for (auto& req : paths) {
    if (!goal.is_allowed(req))
      throw InvalidPath("cannot build '%s' in recursive Nix because path is unknown",
                        req.to_string(*next));
  }

  auto results = next->build_paths_with_results(paths, build_mode);

  for (auto& result : results) {
    if (auto* successP = result.tryGetSuccess()) {
      for (auto& [output_name, output] : successP->built_outputs) {
        new_paths.insert(output.out_path);
        newRealisations.insert(output);
      }
    }
  }

  StorePathSet closure;
  next->computeFSClosure(new_paths, closure);
  for (auto& path : closure)
    goal.addDependency(path);
  for (auto& real : Realisation::closure(*next, newRealisations))
    goal.addedDrvOutputs.insert(real.id);

  return results;
}

MissingPaths restricted_store_t::query_missing(const std::vector<DerivedPath>& targets) {
  /* This is slightly impure since it leaks information to the
     client about what paths will be built/substituted or are
     already present. Probably not a big deal. */

  std::vector<DerivedPath> allowed;
  StorePathSet unknown;
  for (auto& req : targets) {
    if (goal.is_allowed(req))
      allowed.emplace_back(req);
    else
      unknown.insert(path_part_of_req(req));
  }

  auto res = next->query_missing(allowed);

  for (auto& p : unknown)
    res.unknown.insert(p);

  return res;
}

} // namespace nix
