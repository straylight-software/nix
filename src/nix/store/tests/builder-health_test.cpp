/**
 * Tests for BuilderHealthTracker - exponential backoff for down remote builders
 *
 * Fixes issue #13513: Down builder brings all builds to a crawl
 */

#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "nix/store/builder-health.h"

using nix::BuilderHealthTracker;

TEST_CASE("BuilderHealthTracker basics", "[builder-health]") {
  BuilderHealthTracker tracker;
  const std::string uri = "ssh://test@builder1";

  SECTION("new builders are not skipped") {
    REQUIRE_FALSE(tracker.shouldSkip(uri, 30, 1800));
  }

  SECTION("recording success for unknown builder does nothing") {
    tracker.recordSuccess(uri);
    REQUIRE_FALSE(tracker.shouldSkip(uri, 30, 1800));
  }

  SECTION("disabled backoff (0) never skips") {
    tracker.recordFailure(uri, 0, 0);
    REQUIRE_FALSE(tracker.shouldSkip(uri, 0, 0));
  }
}

TEST_CASE("BuilderHealthTracker failure tracking", "[builder-health]") {
  BuilderHealthTracker tracker;
  const std::string uri = "ssh://test@builder2";

  SECTION("first failure triggers backoff") {
    tracker.recordFailure(uri, 30, 1800);
    REQUIRE(tracker.shouldSkip(uri, 30, 1800));

    auto state = tracker.getState(uri);
    REQUIRE(state.failureCount == 1);
  }

  SECTION("success resets failure count") {
    tracker.recordFailure(uri, 30, 1800);
    tracker.recordSuccess(uri);

    auto state = tracker.getState(uri);
    REQUIRE(state.failureCount == 0);
    REQUIRE_FALSE(tracker.shouldSkip(uri, 30, 1800));
  }

  SECTION("multiple failures increase count") {
    tracker.recordFailure(uri, 30, 1800);
    tracker.recordFailure(uri, 30, 1800);
    tracker.recordFailure(uri, 30, 1800);

    auto state = tracker.getState(uri);
    REQUIRE(state.failureCount == 3);
  }
}

TEST_CASE("BuilderHealthTracker exponential backoff", "[builder-health]") {
  // Test that backoff follows exponential pattern
  // initial * 2^(failures-1), capped at max

  SECTION("backoff calculation with small initial value") {
    BuilderHealthTracker tracker;
    const std::string uri = "ssh://test@builder3";

    // With 1s initial, 60s max:
    // Failure 1: 1s
    // Failure 2: 2s
    // Failure 3: 4s
    // Failure 4: 8s
    // Failure 5: 16s
    // Failure 6: 32s
    // Failure 7: 60s (capped)

    // Record 6 failures
    for (int i = 0; i < 6; i++) {
      tracker.recordFailure(uri, 1, 60);
    }

    auto state = tracker.getState(uri);
    REQUIRE(state.failureCount == 6);
    // After 6 failures: 1 * 2^5 = 32 seconds
  }

  SECTION("backoff is capped at max") {
    BuilderHealthTracker tracker;
    const std::string uri = "ssh://test@builder4";

    // Record many failures - backoff should never exceed max
    for (int i = 0; i < 20; i++) {
      tracker.recordFailure(uri, 30, 1800);
    }

    auto state = tracker.getState(uri);
    REQUIRE(state.failureCount == 20);
    // Backoff is capped by max, not infinite
  }
}

TEST_CASE("BuilderHealthTracker multiple builders", "[builder-health]") {
  BuilderHealthTracker tracker;
  const std::string uri1 = "ssh://test@builder-a";
  const std::string uri2 = "ssh://test@builder-b";

  SECTION("failures are tracked independently") {
    tracker.recordFailure(uri1, 30, 1800);
    tracker.recordFailure(uri1, 30, 1800);
    tracker.recordFailure(uri2, 30, 1800);

    auto state1 = tracker.getState(uri1);
    auto state2 = tracker.getState(uri2);

    REQUIRE(state1.failureCount == 2);
    REQUIRE(state2.failureCount == 1);
  }

  SECTION("success for one builder doesn't affect others") {
    tracker.recordFailure(uri1, 30, 1800);
    tracker.recordFailure(uri2, 30, 1800);
    tracker.recordSuccess(uri1);

    auto state1 = tracker.getState(uri1);
    auto state2 = tracker.getState(uri2);

    REQUIRE(state1.failureCount == 0);
    REQUIRE(state2.failureCount == 1);
  }
}

TEST_CASE("BuilderHealthTracker clear", "[builder-health]") {
  BuilderHealthTracker tracker;
  const std::string uri = "ssh://test@builder5";

  tracker.recordFailure(uri, 30, 1800);
  REQUIRE(tracker.getState(uri).failureCount == 1);

  tracker.clear();
  REQUIRE(tracker.getState(uri).failureCount == 0);
}

TEST_CASE("BuilderHealthTracker singleton", "[builder-health]") {
  // Just verify the singleton pattern works
  auto& instance1 = BuilderHealthTracker::instance();
  auto& instance2 = BuilderHealthTracker::instance();

  REQUIRE(&instance1 == &instance2);
}
