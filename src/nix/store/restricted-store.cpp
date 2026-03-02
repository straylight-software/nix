#include "nix/store/restricted-store.h"

#include <sys/wait.h>
#include <unistd.h>

#ifdef __linux__
#  include <sys/prctl.h>
#endif

#include "nix/store/build-result.h"
#include "nix/store/local-store.h"
#include "nix/store/realisation.h"
#include "nix/util/callback.h"
#include "nix/util/file-descriptor.h"
#include "nix/util/finally.h"
#include "nix/util/serialise.h"
#include "nix/util/signals.h"

namespace nix {

static store_path_t path_part_of_req(const SingleDerivedPath& req) {
  return std::visit(
      overloaded{
          [&](const SingleDerivedPath::opaque_t& bo) { return bo.path; },
          [&](const SingleDerivedPath::Built& bfd) { return path_part_of_req(*bfd.drv_path); },
      },
      req.raw());
}

static store_path_t path_part_of_req(const derived_path_t& req) {
  return std::visit(
      overloaded{
          [&](const derived_path_t::opaque_t& bo) { return bo.path; },
          [&](const derived_path_t::Built& bfd) { return path_part_of_req(*bfd.drv_path); },
      },
      req.raw());
}

bool RestrictionContext::is_allowed(const derived_path_t& req) {
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

  restricted_store_t(ref<LocalStore::config_t> config, ref<LocalStore> next,
                     RestrictionContext& goal)
      : store_t{*config}, local_fs_store{*config}, config{config}, next(next), goal(goal) {}

  Path getRealStoreDir() override { return next->config->real_store_dir; }

  store_path_set_t query_all_valid_paths() override;

  void query_path_info_uncached(
      const store_path_t& path,
      Callback<std::shared_ptr<const valid_path_info_t>> callback) noexcept override;

  void query_referrers(const store_path_t& path, store_path_set_t& referrers) override;

  std::map<std::string, std::optional<store_path_t>>
  queryPartialDerivationOutputMap(const store_path_t& path, store_t* eval_store = nullptr) override;

  std::optional<store_path_t> queryPathFromHashPart(const std::string& hash_part) override {
    throw Error("queryPathFromHashPart");
  }

  store_path_t add_to_store(std::string_view name, const source_path_t& src_path,
                            content_address_method_t method, hash_algorithm_t hash_algo,
                            const store_path_set_t& references, path_filter_t& filter,
                            RepairFlag repair) override {
    throw Error("addToStore");
  }

  void add_to_store(const valid_path_info_t& info, source_t& nar_source,
                    RepairFlag repair = NoRepair, CheckSigsFlag check_sigs = CheckSigs) override;

  store_path_t add_to_store_from_dump(source_t& dump, std::string_view name,
                                      file_serialisation_method_t dump_method,
                                      content_address_method_t hash_method,
                                      hash_algorithm_t hash_algo,
                                      const store_path_set_t& references,
                                      RepairFlag repair) override;

  void nar_from_path(const store_path_t& path, sink_t& sink) override;

  void ensure_path(const store_path_t& path) override;

  void register_drv_output(const realisation_t& info) override;

  void query_realisation_uncached(
      const DrvOutput& id,
      Callback<std::shared_ptr<const UnkeyedRealisation>> callback) noexcept override;

  void build_paths(const std::vector<derived_path_t>& paths, BuildMode build_mode,
                   std::shared_ptr<store_t> eval_store) override;

  std::vector<keyed_build_result_t>
  build_paths_with_results(const std::vector<derived_path_t>& paths,
                           BuildMode build_mode = bmNormal,
                           std::shared_ptr<store_t> eval_store = nullptr) override;

  build_result_t buildDerivation(const store_path_t& drv_path, const basic_derivation_t& drv,
                                 BuildMode build_mode = bmNormal) override {
    unsupported("buildDerivation");
  }

  void addTempRoot(const store_path_t& path) override {}

  void addIndirectRoot(const Path& path) override {}

  Roots findRoots(bool censor) override { return Roots(); }

  void collectGarbage(const GCOptions& options, GCResults& results) override {}

  void addSignatures(const store_path_t& store_path, const string_set_t& sigs) override {
    unsupported("addSignatures");
  }

  MissingPaths query_missing(const std::vector<derived_path_t>& targets) override;

  virtual std::optional<std::string> getBuildLogExact(const store_path_t& path) override {
    return std::nullopt;
  }

  virtual void addBuildLog(const store_path_t& path, std::string_view log) override {
    unsupported("addBuildLog");
  }

  std::optional<TrustedFlag> isTrustedClient() override { return NotTrusted; }
};

ref<store_t> make_restricted_store(ref<LocalStore::config_t> config, ref<LocalStore> next,
                                   RestrictionContext& context) {
  return make_ref<restricted_store_t>(config, next, context);
}

store_path_set_t restricted_store_t::query_all_valid_paths() {
  store_path_set_t paths;
  for (auto& p : goal.originalPaths()) {
    paths.insert(p);
  }
  for (auto& p : goal.addedPaths) {
    paths.insert(p);
  }
  return paths;
}

void restricted_store_t::query_path_info_uncached(
    const store_path_t& path,
    Callback<std::shared_ptr<const valid_path_info_t>> callback) noexcept {
  if (goal.is_allowed(path)) {
    try {
      /* Censor impure information. */
      auto info = std::make_shared<valid_path_info_t>(*next->queryPathInfo(path));
      info->deriver.reset();
      info->registrationTime = 0;
      info->ultimate = false;
      info->sigs.clear();
      callback(info);
    } catch (InvalidPath&) {
      callback(nullptr);
    }
  } else {
    callback(nullptr);
  }
};

void restricted_store_t::query_referrers(const store_path_t& path, store_path_set_t& referrers) {}

std::map<std::string, std::optional<store_path_t>>
restricted_store_t::queryPartialDerivationOutputMap(const store_path_t& path, store_t* eval_store) {
  if (!goal.is_allowed(path)) {
    throw InvalidPath("cannot query output map for unknown path '%s' in recursive Nix",
                      printStorePath(path));
  }
  return next->queryPartialDerivationOutputMap(path, eval_store);
}

void restricted_store_t::add_to_store(const valid_path_info_t& info, source_t& nar_source,
                                      RepairFlag repair, CheckSigsFlag check_sigs) {
  next->add_to_store(info, nar_source, repair, check_sigs);
  goal.addDependency(info.path);
}

store_path_t restricted_store_t::add_to_store_from_dump(source_t& dump, std::string_view name,
                                                        file_serialisation_method_t dump_method,
                                                        content_address_method_t hash_method,
                                                        hash_algorithm_t hash_algo,
                                                        const store_path_set_t& references,
                                                        RepairFlag repair) {
  auto path = next->add_to_store_from_dump(dump, name, dump_method, hash_method, hash_algo,
                                           references, repair);
  goal.addDependency(path);
  return path;
}

void restricted_store_t::nar_from_path(const store_path_t& path, sink_t& sink) {
  if (!goal.is_allowed(path)) {
    throw InvalidPath("cannot dump unknown path '%s' in recursive Nix", printStorePath(path));
  }
  store_t::nar_from_path(path, sink);
}

void restricted_store_t::ensure_path(const store_path_t& path) {
  if (!goal.is_allowed(path)) {
    throw InvalidPath("cannot substitute unknown path '%s' in recursive Nix", printStorePath(path));
  }
  /* Nothing to be done; 'path' must already be valid. */
}

void restricted_store_t::register_drv_output(const realisation_t& info)
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
  if (!goal.is_allowed(id)) {
    callback(nullptr);
  }
  next->query_realisation(id, std::move(callback));
}

void restricted_store_t::build_paths(const std::vector<derived_path_t>& paths, BuildMode build_mode,
                                     std::shared_ptr<store_t> eval_store) {
  for (auto& result : build_paths_with_results(paths, build_mode, eval_store)) {
    if (auto* failureP = result.tryGetFailure()) {
      failureP->rethrow();
    }
  }
}

/**
 * Helper to serialize build results to a sink for IPC.
 *
 * Format:
 * - count: uint64
 * - for each result:
 *   - path: string (derived path in store format)
 *   - timesBuilt: uint64
 *   - start_time: uint64
 *   - stopTime: uint64
 *   - isSuccess: uint64 (1 = success, 0 = failure)
 *   - if success:
 *     - status: uint64
 *     - outputCount: uint64
 *     - for each output:
 *       - name: string
 *       - id: string (DrvOutput format)
 *       - outPath: string (store path)
 *       - dependentRealisationsCount: uint64
 *       - for each dependent:
 *         - depId: string (DrvOutput)
 *         - depPath: string (store path)
 *   - if failure:
 *     - status: uint64
 *     - errorMsg: string
 *     - isNonDeterministic: uint64 (0 or 1)
 */
static void write_build_results(sink_t& sink, const store_t& store,
                                const std::vector<keyed_build_result_t>& results) {
  sink << (uint64_t)results.size();
  for (const auto& result : results) {
    // Serialize the path
    sink << result.path.to_string(store);
    // keyed_build_result_t inherits from build_result_t, so access directly
    sink << (uint64_t)result.timesBuilt;
    sink << (uint64_t)result.start_time;
    sink << (uint64_t)result.stopTime;

    // Serialize success/failure status
    if (auto* successP = result.tryGetSuccess()) {
      sink << (uint64_t)1; // success marker
      sink << (uint64_t)static_cast<int>(successP->status);
      sink << (uint64_t)successP->built_outputs.size();
      for (const auto& [name, real] : successP->built_outputs) {
        sink << name;
        sink << real.id.to_string();
        sink << store.printStorePath(real.out_path);
        // Serialize dependentRealisations
        sink << (uint64_t)real.dependentRealisations.size();
        for (const auto& [depId, depPath] : real.dependentRealisations) {
          sink << depId.to_string();
          sink << store.printStorePath(depPath);
        }
      }
    } else if (auto* failureP = result.tryGetFailure()) {
      sink << (uint64_t)0; // failure marker
      sink << (uint64_t)static_cast<int>(failureP->status);
      sink << failureP->errorMsg;
      sink << (uint64_t)(failureP->isNonDeterministic ? 1 : 0);
    }
  }
}

/**
 * Helper to deserialize build results from a source for IPC.
 */
static std::vector<keyed_build_result_t> read_build_results(source_t& source, store_t& store) {
  std::vector<keyed_build_result_t> results;
  auto count = read_num<uint64_t>(source);
  results.reserve(count);

  for (uint64_t i = 0; i < count; ++i) {
    // Deserialize the path
    auto pathStr = read_string(source);
    auto path = derived_path_t::parse(store, pathStr);

    // Start building the build_result_t
    build_result_t br;
    br.timesBuilt = read_num<uint64_t>(source);
    br.start_time = read_num<uint64_t>(source);
    br.stopTime = read_num<uint64_t>(source);

    auto isSuccess = read_num<uint64_t>(source);
    if (isSuccess) {
      build_result_t::Success success;
      success.status = static_cast<build_result_t::Success::Status>(read_num<uint64_t>(source));
      auto outputCount = read_num<uint64_t>(source);
      for (uint64_t j = 0; j < outputCount; ++j) {
        auto name = read_string(source);
        auto idStr = read_string(source);
        auto outPathStr = read_string(source);

        // Read dependentRealisations
        std::map<DrvOutput, store_path_t> depReals;
        auto depCount = read_num<uint64_t>(source);
        for (uint64_t k = 0; k < depCount; ++k) {
          auto depIdStr = read_string(source);
          auto depPathStr = read_string(source);
          depReals.emplace(DrvOutput::parse(depIdStr), store.parseStorePath(depPathStr));
        }

        realisation_t real{
            {store.parseStorePath(outPathStr), {}, std::move(depReals)},
            DrvOutput::parse(idStr),
        };
        success.built_outputs.emplace(name, std::move(real));
      }
      br.inner = std::move(success);
    } else {
      build_result_t::Failure failure;
      failure.status = static_cast<build_result_t::Failure::Status>(read_num<uint64_t>(source));
      failure.errorMsg = read_string(source);
      failure.isNonDeterministic = read_num<uint64_t>(source) != 0;
      br.inner = std::move(failure);
    }

    results.emplace_back(std::move(br), std::move(path));
  }
  return results;
}

std::vector<keyed_build_result_t>
restricted_store_t::build_paths_with_results(const std::vector<derived_path_t>& paths,
                                             BuildMode build_mode,
                                             std::shared_ptr<store_t> eval_store) {
  assert(!eval_store);

  if (build_mode != bmNormal) {
    throw Error("unsupported build mode");
  }

  for (auto& req : paths) {
    if (!goal.is_allowed(req)) {
      throw InvalidPath("cannot build '%s' in recursive Nix because path is unknown",
                        req.to_string(*next));
    }
  }

  /*
   * DEADLOCK FIX (NixOS/nix#4216):
   *
   * The recursive Nix daemon runs in threads spawned by the outer builder.
   * If we call next->build_paths_with_results() directly, it creates a new
   * Worker that blocks waiting for builds. But those builds may need the
   * outer Worker to process them, and the outer Worker is blocked waiting
   * for the builder output. This creates a circular wait = deadlock.
   *
   * Solution: Fork a child process to perform the build. The child has its
   * own Worker that can run independently without blocking the outer daemon.
   * Results are serialized back to the parent via a pipe.
   */

  // Create pipe for results. Use O_CLOEXEC to prevent pipe FDs from being
  // inherited by grandchildren (e.g., builders spawned by the recursive build).
  pipe_t result_pipe;
  result_pipe.create(); // Uses pipe2(O_CLOEXEC)

  pid_t pid = fork();
  if (pid == -1) {
    throw sys_error_t("forking for recursive Nix build");
  }

  if (pid == 0) {
    // Child process
    try {
      // Ensure child dies if parent dies. This prevents orphaned processes
      // from holding lock file descriptors indefinitely (NixOS/nix#12142).
#ifdef __linux__
      if (prctl(PR_SET_PDEATHSIG, SIGKILL) == -1) {
        throw sys_error_t("setting death signal for recursive Nix build");
      }
#endif

      result_pipe.read_side.close();

      // Perform the actual build in the child process
      // This creates a new Worker that won't deadlock because it's independent
      auto results = next->build_paths_with_results(paths, build_mode);

      // Serialize results back to parent
      fd_sink_t sink(result_pipe.write_side.get());
      sink << (uint64_t)0; // success marker
      write_build_results(sink, *next, results);
      sink.flush();

      _exit(0);
    } catch (const std::exception& e) {
      try {
        fd_sink_t sink(result_pipe.write_side.get());
        sink << (uint64_t)1; // error marker
        sink << std::string(e.what());
        sink.flush();
      } catch (...) {
      }
      _exit(1);
    } catch (...) {
      _exit(1);
    }
  }

  // Parent process
  result_pipe.write_side.close();

  // Read results from child
  fd_source_t source(result_pipe.read_side.get());

  std::vector<keyed_build_result_t> results;
  try {
    auto status_marker = read_num<uint64_t>(source);
    if (status_marker != 0) {
      // Child reported an error
      auto errMsg = read_string(source);
      // Wait for child to exit
      int status;
      waitpid(pid, &status, 0);
      throw Error("recursive Nix build failed: %s", errMsg);
    }
    results = read_build_results(source, *next);
  } catch (EndOfFile&) {
    // Child died unexpectedly, wait and report
    int status;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status)) {
      throw Error("recursive Nix build process exited with status %d", WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
      throw Error("recursive Nix build process killed by signal %d", WTERMSIG(status));
    } else {
      throw Error("recursive Nix build process terminated unexpectedly");
    }
  }

  // Wait for child to finish
  int status;
  if (waitpid(pid, &status, 0) == -1) {
    throw sys_error_t("waiting for recursive Nix build process");
  }

  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    if (WIFEXITED(status)) {
      throw Error("recursive Nix build process exited with status %d", WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
      throw Error("recursive Nix build process killed by signal %d", WTERMSIG(status));
    }
  }

  // Now update addedPaths and addedDrvOutputs in the parent process
  store_path_set_t new_paths;
  std::set<realisation_t> newRealisations;

  for (auto& result : results) {
    if (auto* successP = result.tryGetSuccess()) {
      for (auto& [output_name, output] : successP->built_outputs) {
        new_paths.insert(output.out_path);
        newRealisations.insert(output);
      }
    }
  }

  store_path_set_t closure;
  next->computeFSClosure(new_paths, closure);
  for (auto& path : closure) {
    goal.addDependency(path);
  }
  for (auto& real : realisation_t::closure(*next, newRealisations)) {
    goal.addedDrvOutputs.insert(real.id);
  }

  return results;
}

MissingPaths restricted_store_t::query_missing(const std::vector<derived_path_t>& targets) {
  /* This is slightly impure since it leaks information to the
     client about what paths will be built/substituted or are
     already present. Probably not a big deal. */

  std::vector<derived_path_t> allowed;
  store_path_set_t unknown;
  for (auto& req : targets) {
    if (goal.is_allowed(req)) {
      allowed.emplace_back(req);
    } else {
      unknown.insert(path_part_of_req(req));
    }
  }

  auto res = next->query_missing(allowed);

  for (auto& p : unknown) {
    res.unknown.insert(p);
  }

  return res;
}

} // namespace nix
