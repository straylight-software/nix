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
  DerivationBuildingGoal(const store_path_t& drv_path, const derivation_t& drv, Worker& worker,
                         BuildMode build_mode, bool storeDerivation);
  ~DerivationBuildingGoal();

private:
  /** The path of the derivation. */
  store_path_t drv_path;

  /**
   * The derivation stored at drv_path.
   */
  std::unique_ptr<derivation_t> drv;

  /**
   * The remainder is state held during the build.
   */

  /**
   * All input paths (that is, the union of FS closures of the
   * immediate input paths).
   */
  store_path_set_t inputPaths;

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

  BuildMode build_mode;

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
                         const derivation_options_t<store_path_t>& drv_options);

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
  void handle_eof(descriptor_t fd) override;
  void flush_line();

  /**
   * Wrappers around the corresponding store_t methods that first consult the
   * derivation.  This is currently needed because when there is no drv file
   * there also is no DB entry.
   */
  std::map<std::string, std::optional<store_path_t>> queryPartialDerivationOutputMap();

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
  void kill_child();

  done_t doneSuccess(build_result_t::Success::Status status, SingleDrvOutputs built_outputs);

  done_t doneFailure(build_error_t ex);

  build_error_t fixupBuilderFailureErrorMessage(BuilderFailureError msg);

  JobCategory jobCategory() const override { return JobCategory::Build; };
};

} // namespace nix
