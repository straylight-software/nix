#pragma once
///@file

#include <chrono>
#include <optional>
#include <string>

#include "nix/store/derived-path.h"
#include "nix/store/realisation.h"
#include "nix/util/json-impls.h"

namespace nix {

struct build_result_t {
  struct Success {
    /**
     * @note This is directly used in the nix-store --serve protocol.
     * That means we need to worry about compatibility across versions.
     * Therefore, don't remove status codes, and only add new status
     * codes at the end of the list.
     *
     * Must be disjoint with `Failure::Status`.
     */
    enum Status : uint8_t {
      Built = 0,
      Substituted = 1,
      AlreadyValid = 2,
      ResolvesToAlreadyValid = 13,
    } status;

    static std::string_view status_to_string(Status status);

    /**
     * For derivations, a mapping from the names of the wanted outputs
     * to actual paths.
     */
    SingleDrvOutputs built_outputs;

    bool operator==(const build_result_t::Success&) const noexcept;
    std::strong_ordering operator<=>(const build_result_t::Success&) const noexcept;

    static bool statusIs(uint8_t status) {
      return status == Built || status == Substituted || status == AlreadyValid ||
             status == ResolvesToAlreadyValid;
    }
  };

  struct Failure {
    /**
     * @note This is directly used in the nix-store --serve protocol.
     * That means we need to worry about compatibility across versions.
     * Therefore, don't remove status codes, and only add new status
     * codes at the end of the list.
     *
     * Must be disjoint with `Success::Status`.
     */
    enum Status : uint8_t {
      PermanentFailure = 3,
      InputRejected = 4,
      OutputRejected = 5,
      /// possibly transient
      TransientFailure = 6,
      /// no longer used
      CachedFailure = 7,
      TimedOut = 8,
      MiscFailure = 9,
      DependencyFailed = 10,
      LogLimitExceeded = 11,
      not_deterministic_t = 12,
      NoSubstituters = 14,
      /// A certain type of `OutputRejected`. The protocols do not yet
      /// know about this one, so change it back to `OutputRejected`
      /// before serialization.
      HashMismatch = 15,
      Cancelled = 16,
    } status = MiscFailure;

    static std::string_view status_to_string(Status status);

    /**
     * Information about the error if the build failed.
     *
     * @todo This should be an entire error_info_t object, not just a
     * string, for richer information.
     */
    std::string errorMsg;

    /**
     * If timesBuilt > 1, whether some builds did not produce the same
     * result. (Note that 'isNonDeterministic = false' does not mean
     * the build is deterministic, just that we don't have evidence of
     * non-determinism.)
     */
    bool isNonDeterministic = false;

    bool operator==(const build_result_t::Failure&) const noexcept;
    std::strong_ordering operator<=>(const build_result_t::Failure&) const noexcept;

    [[noreturn]] void rethrow() const {
      throw Error("%s", errorMsg.empty() ? status_to_string(status) : errorMsg);
    }
  };

  std::variant<Success, Failure> inner = Failure{};

  /**
   * Convenience wrapper to avoid a longer `std::get_if` usage by the
   * caller (which will have to add more `build_result_t::` than we do
   * below also, do note.)
   */
  auto* tryGetSuccess(this auto& self) { return std::get_if<Success>(&self.inner); }

  /**
   * Convenience wrapper to avoid a longer `std::get_if` usage by the
   * caller (which will have to add more `build_result_t::` than we do
   * below also, do note.)
   */
  auto* tryGetFailure(this auto& self) { return std::get_if<Failure>(&self.inner); }

  /**
   * How many times this build was performed.
   */
  unsigned int timesBuilt = 0;

  /**
   * The start/stop times of the build (or one of the rounds, if it
   * was repeated).
   */
  time_t start_time = 0, stopTime = 0;

  /**
   * User and system CPU time the build took.
   */
  std::optional<std::chrono::microseconds> cpu_user, cpu_system;

  bool operator==(const build_result_t&) const noexcept;
  std::strong_ordering operator<=>(const build_result_t&) const noexcept;

  bool isCancelled() const {
    auto failure = tryGetFailure();
    // FIXME: remove MiscFailure eventually.
    return failure &&
           (failure->status == Failure::Cancelled || failure->status == Failure::MiscFailure);
  }
};

/**
 * denotes a permanent build failure
 */
struct build_error_t : public Error {
  build_result_t::Failure::Status status;

  build_error_t(build_result_t::Failure::Status status, auto&&... args)
      : Error{args...}, status{status} {}
};

/**
 * A `build_result_t` together with its "primary key".
 */
struct keyed_build_result_t : build_result_t {
  /**
   * The derivation we built or the store path we substituted.
   */
  derived_path_t path;

  // Hack to work around a gcc "may be used uninitialized" warning.
  keyed_build_result_t(build_result_t res, derived_path_t path)
      : build_result_t(std::move(res)), path(std::move(path)) {}
};

} // namespace nix

JSON_IMPL(nix::build_result_t)
JSON_IMPL(nix::keyed_build_result_t)
