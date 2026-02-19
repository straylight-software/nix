#pragma once
///@file

#include "nix/store/build/derivation-building-misc.h"
#include "nix/store/build/goal.h"
#include "nix/store/derivation-options.h"
#include "nix/store/derivations.h"
#include "nix/store/outputs-spec.h"
#include "nix/store/parsed-derivations.h"
#include "nix/store/pathlocks.h"
#include "nix/store/store-api.h"

namespace nix {

using std::map;

struct BuilderFailureError;
#ifndef _WIN32 // TODO enable build hook on Windows
struct HookInstance;
struct DerivationBuilder;
#endif

typedef enum { rpAccept, rpDecline, rpPostpone } HookReply;

/**
 * A goal for building a derivation. Substitution, (or any other method of
 * obtaining the outputs) will not be attempted, so it is the calling goal's
 * responsibility to try to substitute first.
 */
struct DerivationBuildingGoal : public Goal {
  /**
   * @param storeDerivation Whether to store the derivation in
   * `worker.store`. This is useful for newly-resolved derivations. In this
   * case, the derivation was not created a priori, e.g. purely (or close
   * enough) from evaluation of the Nix language, but also depends on the
   * exact content produced by upstream builds. It is strongly advised to
   * have a permanent record of such a resolved derivation in order to
   * faithfully reconstruct the build history.
   */
  DerivationBuildingGoal(const StorePath& drvPath, const Derivation& drv, Worker& worker,
                         BuildMode buildMode, bool storeDerivation);
  ~DerivationBuildingGoal();

private:
  /** The path of the derivation. */
  StorePath drvPath;

  /**
   * The derivation stored at drvPath.
   */
  std::unique_ptr<Derivation> drv;

  /**
   * The remainder is state held during the build.
   */

  /**
   * All input paths (that is, the union of FS closures of the
   * immediate input paths).
   */
  StorePathSet inputPaths;

  /**
   * file_t descriptor for the log file.
   */
  auto_close_fd_t fdLogFile;
  std::shared_ptr<buffered_sink_t> logFileSink, logSink;

  /**
   * Number of bytes received from the builder's stdout/stderr.
   */
  unsigned long logSize;

  /**
   * The most recent log lines.
   */
  std::list<std::string> logTail;

  std::string currentLogLine;
  size_t currentLogLinePos = 0; // to handle carriage return

  std::string currentHookLine;

#ifndef _WIN32 // TODO enable build hook on Windows
  /**
   * The build hook.
   */
  std::unique_ptr<HookInstance> hook;

  std::unique_ptr<DerivationBuilder> builder;
#endif

  BuildMode buildMode;

  std::unique_ptr<maintain_count_t<uint64_t>> mcRunningBuilds;

  std::unique_ptr<activity_t> act;

  std::map<activity_id_t, activity_t> builderActivities;

  void timedOut(Error&& ex) override;

  std::string key() override;

  /**
   * The states.
   */
  Co gaveUpOnSubstitution(bool storeDerivation);
  Co tryToBuild();

  /**
   * Is the build hook willing to perform the build?
   */
  HookReply tryBuildHook(const std::map<std::string, InitialOutput>& initialOutputs,
                         const DerivationOptions<StorePath>& drvOptions);

  /**
   * Open a log file and a pipe to it.
   */
  Path openLogFile();

  /**
   * Close the log file.
   */
  void closeLogFile();

  bool isReadDesc(descriptor_t fd);

  /**
   * Callback used by the worker to write to the log.
   */
  void handleChildOutput(descriptor_t fd, std::string_view data) override;
  void handleEOF(descriptor_t fd) override;
  void flushLine();

  /**
   * Wrappers around the corresponding Store methods that first consult the
   * derivation.  This is currently needed because when there is no drv file
   * there also is no DB entry.
   */
  std::map<std::string, std::optional<StorePath>> queryPartialDerivationOutputMap();

  /**
   * Update 'initialOutputs' to determine the current status of the
   * outputs of the derivation. Also returns a Boolean denoting
   * whether all outputs are valid and non-corrupt, and a
   * 'SingleDrvOutputs' structure containing the valid outputs.
   */
  std::pair<bool, SingleDrvOutputs>
  checkPathValidity(std::map<std::string, InitialOutput>& initialOutputs);

  /**
   * Forcibly kill the child process, if any.
   */
  void killChild();

  done_t doneSuccess(BuildResult::Success::Status status, SingleDrvOutputs builtOutputs);

  done_t doneFailure(BuildError ex);

  BuildError fixupBuilderFailureErrorMessage(BuilderFailureError msg);

  JobCategory jobCategory() const override { return JobCategory::Build; };
};

} // namespace nix
