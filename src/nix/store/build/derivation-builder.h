#pragma once
///@file

#include <nlohmann/json_fwd.hpp>

#include "nix/store/build-result.h"
#include "nix/store/build/derivation-building-misc.h"
#include "nix/store/build/derivation-env-desugar.h"
#include "nix/store/derivation-options.h"
#include "nix/store/derivations.h"
#include "nix/store/parsed-derivations.h"
#include "nix/store/restricted-store.h"
#include "nix/util/json-impls.h"
#include "nix/util/processes.h"

namespace nix {

/**
 * Denotes a build failure that stemmed from the builder exiting with a
 * failing exist status.
 */
struct BuilderFailureError : build_error_t {
  int builderStatus;

  std::string extraMsgAfter;

  BuilderFailureError(build_result_t::Failure::Status status, int builderStatus, std::string extraMsgAfter)
        : build_error_t{
            status,
              /* No message for now, because the caller will make for
                 us, with extra context */
              "",
          }
        , builderStatus{std::move(builderStatus)}
        , extraMsgAfter{std::move(extraMsgAfter)}
    {
    }
};

/**
 * Stuff we need to pass to initChild().
 */
struct ChrootPath {
  Path source;
  bool optional = false;
};

typedef std::map<Path, ChrootPath> PathsInChroot; // maps target path to source path

/**
 * Parameters by (mostly) `const` reference for `DerivationBuilder`.
 */
struct DerivationBuilderParams {
  /** The path of the derivation. */
  const store_path_t& drv_path;

  build_result_t& buildResult;

  /**
   * The derivation stored at drv_path.
   */
  const basic_derivation_t& drv;

  /**
   * The derivation options of `drv`.
   *
   * @todo this should be part of `derivation_t`.
   */
  const derivation_options_t<store_path_t>& drv_options;

  // The remainder is state held during the build.

  /**
   * All input paths (that is, the union of FS closures of the
   * immediate input paths).
   */
  const store_path_set_t& inputPaths;

  const std::map<std::string, InitialOutput> initialOutputs;

  const BuildMode& build_mode;

  /**
   * Extra paths we want to be in the chroot, regardless of the
   * derivation we are building.
   */
  PathsInChroot defaultPathsInChroot;

  /**
   * May be used to control various platform-specific functionality.
   *
   * For example, on Linux, the `kvm` system feature controls whether
   * `/dev/kvm` should be exposed to the builder within the sandbox.
   */
  string_set_t systemFeatures;

  DesugaredEnv desugaredEnv;

  /**
   * The activity corresponding to the build.
   */
  std::unique_ptr<activity_t>& act;
};

/**
 * Callbacks that `DerivationBuilder` needs.
 */
struct DerivationBuilderCallbacks {
  virtual ~DerivationBuilderCallbacks() = default;

  /**
   * Open a log file and a pipe to it.
   */
  virtual Path openLogFile() = 0;

  /**
   * Close the log file.
   */
  virtual void closeLogFile() = 0;

  /**
   * @todo this should be reworked
   */
  virtual void childTerminated() = 0;
};

/**
 * This class represents the state for building locally.
 *
 * @todo Ideally, it would not be a class, but a single function.
 * However, besides the main entry point, there are a few more methods
 * which are externally called, and need to be gotten rid of. There are
 * also some virtual methods (either directly here or inherited from
 * `DerivationBuilderCallbacks`, a stop-gap) that represent outgoing
 * rather than incoming call edges that either should be removed, or
 * become (higher order) function parameters.
 */
struct DerivationBuilder : RestrictionContext {
  DerivationBuilder() = default;
  virtual ~DerivationBuilder() = default;

  /**
   * Master side of the pseudoterminal used for the builder's
   * standard output/error.
   */
  auto_close_fd_t builder_out;

  /**
   * Set up build environment / sandbox, acquiring resources (e.g.
   * locks as needed). After this is run, the builder should be
   * started.
   *
   * @returns logging pipe if successful, `std::nullopt` if we could
   * not acquire a build user. In that case, the caller must wait and
   * then try again.
   *
   * @note "success" just means that we were able to set up the environment
   * and start the build. The builder could have immediately exited with
   * failure, and that would still be considered a successful start.
   */
  virtual std::optional<descriptor_t> start_build() = 0;

  /**
   * Tear down build environment after the builder exits (either on
   * its own or if it is killed).
   *
   * @returns The first case indicates failure during output
   * processing. A status code and exception are returned, providing
   * more information. The second case indicates success, and
   * realisations for each output of the derivation are returned.
   *
   * @throws build_error_t
   */
  virtual SingleDrvOutputs unprepare_build() = 0;

  /**
   * Forcibly kill the child process, if any.
   *
   * @returns whether the child was still alive and needed to be
   * killed.
   */
  virtual bool kill_child() = 0;
};

struct ExternalBuilder {
  string_set_t systems;
  Path program;
  std::vector<std::string> args;
};

#ifndef _WIN32 // TODO enable `DerivationBuilder` on Windows
std::unique_ptr<DerivationBuilder>
make_derivation_builder(LocalStore& store, std::unique_ptr<DerivationBuilderCallbacks> misc_methods,
                        DerivationBuilderParams params);

/**
 * @param handler Must be chosen such that it supports the given
 * derivation.
 */
std::unique_ptr<DerivationBuilder>
make_external_derivation_builder(LocalStore& store,
                                 std::unique_ptr<DerivationBuilderCallbacks> misc_methods,
                                 DerivationBuilderParams params, const ExternalBuilder& handler);
#endif

} // namespace nix

JSON_IMPL(nix::ExternalBuilder)
