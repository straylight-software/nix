#pragma once
///@file

#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>

namespace nix {

/**
 * Tracks health status of remote builders to implement exponential backoff
 * for unreachable/down builders. This prevents a single down builder from
 * slowing down all builds by repeatedly attempting failed connections.
 *
 * Thread-safe for concurrent access from multiple build requests.
 */
class BuilderHealthTracker {
public:
  using Clock = std::chrono::steady_clock;
  using TimePoint = Clock::time_point;

  struct BuilderState {
    /// Number of consecutive failures
    unsigned int failureCount = 0;

    /// Time of last failure
    TimePoint lastFailure;

    /// Time until which this builder should be skipped
    TimePoint skipUntil;
  };

  /**
   * Check if a builder should be skipped due to recent failures.
   *
   * @param builderUri The URI identifying the builder
   * @param initialBackoffSeconds Initial backoff duration after first failure
   * @param maxBackoffSeconds Maximum backoff duration
   * @return true if the builder should be skipped, false if it can be tried
   */
  bool shouldSkip(const std::string& builderUri, unsigned int initialBackoffSeconds,
                  unsigned int maxBackoffSeconds);

  /**
   * Record a connection failure for a builder.
   * Increases the failure count and calculates the next backoff period.
   *
   * @param builderUri The URI identifying the builder
   * @param initialBackoffSeconds Initial backoff duration after first failure
   * @param maxBackoffSeconds Maximum backoff duration
   */
  void recordFailure(const std::string& builderUri, unsigned int initialBackoffSeconds,
                     unsigned int maxBackoffSeconds);

  /**
   * Record a successful connection to a builder.
   * Resets the failure count and backoff state.
   *
   * @param builderUri The URI identifying the builder
   */
  void recordSuccess(const std::string& builderUri);

  /**
   * Get the current state of a builder (for diagnostics/logging).
   *
   * @param builderUri The URI identifying the builder
   * @return The builder state, or a default state if not tracked
   */
  BuilderState getState(const std::string& builderUri) const;

  /**
   * Clear all tracked state (useful for testing or manual reset).
   */
  void clear();

  /**
   * Get the singleton instance.
   */
  static BuilderHealthTracker& instance();

private:
  mutable std::mutex mutex_;
  std::unordered_map<std::string, BuilderState> states_;

  /**
   * Calculate backoff duration based on failure count.
   * Uses exponential backoff: initial * 2^(failures-1), capped at max.
   */
  static std::chrono::seconds calculateBackoff(unsigned int failureCount,
                                               unsigned int initialBackoffSeconds,
                                               unsigned int maxBackoffSeconds);
};

} // namespace nix
