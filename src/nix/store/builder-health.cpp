#include "nix/store/builder-health.h"

#include <algorithm>

#include "nix/util/logging.h"

namespace nix {

BuilderHealthTracker& BuilderHealthTracker::instance() {
  static BuilderHealthTracker tracker;
  return tracker;
}

bool BuilderHealthTracker::shouldSkip(const std::string& builderUri,
                                      unsigned int initialBackoffSeconds,
                                      unsigned int maxBackoffSeconds) {
  // If backoff is disabled, never skip
  if (initialBackoffSeconds == 0) {
    return false;
  }

  std::lock_guard<std::mutex> lock(mutex_);

  auto it = states_.find(builderUri);
  if (it == states_.end()) {
    return false; // No failure history, don't skip
  }

  const auto& state = it->second;
  if (state.failureCount == 0) {
    return false; // No failures recorded
  }

  auto now = Clock::now();
  if (now < state.skipUntil) {
    auto remaining =
        std::chrono::duration_cast<std::chrono::seconds>(state.skipUntil - now).count();
    debug("skipping builder '%s' for %d more seconds (failure count: %d)", builderUri, remaining,
          state.failureCount);
    return true;
  }

  return false;
}

void BuilderHealthTracker::recordFailure(const std::string& builderUri,
                                         unsigned int initialBackoffSeconds,
                                         unsigned int maxBackoffSeconds) {
  // If backoff is disabled, don't track
  if (initialBackoffSeconds == 0) {
    return;
  }

  std::lock_guard<std::mutex> lock(mutex_);

  auto& state = states_[builderUri];
  state.failureCount++;
  state.lastFailure = Clock::now();

  auto backoff = calculateBackoff(state.failureCount, initialBackoffSeconds, maxBackoffSeconds);
  state.skipUntil = state.lastFailure + backoff;

  warn("builder '%s' connection failed (attempt %d), backing off for %d seconds", builderUri,
       state.failureCount, static_cast<int>(backoff.count()));
}

void BuilderHealthTracker::recordSuccess(const std::string& builderUri) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = states_.find(builderUri);
  if (it != states_.end() && it->second.failureCount > 0) {
    debug("builder '%s' recovered after %d failures", builderUri, it->second.failureCount);
    it->second.failureCount = 0;
    it->second.skipUntil = TimePoint{}; // Reset skip time
  }
}

BuilderHealthTracker::BuilderState
BuilderHealthTracker::getState(const std::string& builderUri) const {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = states_.find(builderUri);
  if (it != states_.end()) {
    return it->second;
  }
  return BuilderState{};
}

void BuilderHealthTracker::clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  states_.clear();
}

std::chrono::seconds BuilderHealthTracker::calculateBackoff(unsigned int failureCount,
                                                            unsigned int initialBackoffSeconds,
                                                            unsigned int maxBackoffSeconds) {
  if (failureCount == 0) {
    return std::chrono::seconds{0};
  }

  // Exponential backoff: initial * 2^(failures-1)
  // Cap the exponent to avoid overflow (2^10 = 1024 is plenty)
  unsigned int exponent = std::min(failureCount - 1, 10u);
  unsigned int backoffSeconds = initialBackoffSeconds * (1u << exponent);

  // Cap at max
  backoffSeconds = std::min(backoffSeconds, maxBackoffSeconds);

  return std::chrono::seconds{backoffSeconds};
}

} // namespace nix
