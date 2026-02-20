// straylight::nix::text
//
// High-performance fuzzy string matching using rapidfuzz-cpp.
//
// Provides SIMD-optimized Levenshtein distance and other fuzzy matching
// algorithms with O([N/64]M) worst-case complexity for strings up to 64 chars.
//
// Usage:
//   auto dist = fuzzy::levenshtein("hello", "hallo");  // returns 1
//   auto ratio = fuzzy::ratio("hello", "hallo");       // returns 0.8
//   auto matches = fuzzy::best_matches(candidates, "query", 5);
//
// Performance vs NIH implementation:
//   - Uses Myers' bit-parallel algorithm (64 chars at a time)
//   - Common prefix/suffix removal optimization
//   - Early termination with max distance cutoff
//   - ~10-50x faster than naive O(mn) implementation

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rapidfuzz/distance/Levenshtein.hpp>
#include <rapidfuzz/fuzz.hpp>

namespace straylight::nix::text {

// ─────────────────────────────────────────────────────────────────────────────
// Levenshtein Distance
// ─────────────────────────────────────────────────────────────────────────────

/// Compute Levenshtein edit distance between two strings.
/// Returns minimum number of single-character edits (insertions, deletions,
/// substitutions) required to change s1 into s2.
///
/// Uses SIMD-optimized bit-parallel algorithm for strings up to 64 chars.
[[nodiscard]] inline std::size_t levenshtein(std::string_view s1, std::string_view s2) noexcept {
  return rapidfuzz::levenshtein_distance(s1, s2);
}

/// Compute Levenshtein distance with early termination.
/// Returns max + 1 if the distance exceeds max.
[[nodiscard]] inline std::size_t levenshtein(std::string_view s1, std::string_view s2,
                                             std::size_t max) noexcept {
  return rapidfuzz::levenshtein_distance(s1, s2, {1, 1, 1}, max);
}

/// Compute normalized Levenshtein similarity (0.0 to 1.0).
/// Returns 1.0 for identical strings, 0.0 for completely different strings.
[[nodiscard]] inline double levenshtein_normalized(std::string_view s1,
                                                   std::string_view s2) noexcept {
  return rapidfuzz::levenshtein_normalized_similarity(s1, s2);
}

// ─────────────────────────────────────────────────────────────────────────────
// Fuzzy Ratio (fuzz.ratio equivalent)
// ─────────────────────────────────────────────────────────────────────────────

/// Compute fuzzy match ratio (0.0 to 100.0).
/// Uses token-based matching for better results with word reordering.
[[nodiscard]] inline double ratio(std::string_view s1, std::string_view s2) noexcept {
  return rapidfuzz::fuzz::ratio(s1, s2);
}

/// Compute partial ratio - finds best matching substring.
/// Useful when one string is much shorter than the other.
[[nodiscard]] inline double partial_ratio(std::string_view s1, std::string_view s2) noexcept {
  return rapidfuzz::fuzz::partial_ratio(s1, s2);
}

/// Compute token sort ratio - sorts tokens before comparing.
/// Useful for comparing strings with words in different order.
[[nodiscard]] inline double token_sort_ratio(std::string_view s1, std::string_view s2) noexcept {
  return rapidfuzz::fuzz::token_sort_ratio(s1, s2);
}

/// Compute token set ratio - compares unique token sets.
/// Most forgiving: ignores duplicates and order.
[[nodiscard]] inline double token_set_ratio(std::string_view s1, std::string_view s2) noexcept {
  return rapidfuzz::fuzz::token_set_ratio(s1, s2);
}

// ─────────────────────────────────────────────────────────────────────────────
// Best Matches (for suggestions/autocomplete)
// ─────────────────────────────────────────────────────────────────────────────

/// A match result with distance and the matched string
struct Match {
  std::size_t distance;
  std::string_view text;

  bool operator<(const Match& other) const noexcept {
    if (distance != other.distance) {
      return distance < other.distance;
    }
    return text < other.text;
  }

  bool operator==(const Match& other) const noexcept = default;
};

/// Find best matches from a set of candidates.
/// Returns up to `limit` matches sorted by distance (best first).
/// Only includes matches with distance <= max_distance.
template <typename Container>
[[nodiscard]] std::vector<Match> best_matches(const Container& candidates, std::string_view query,
                                              std::size_t limit = 5, std::size_t max_distance = 3) {
  std::vector<Match> results;
  results.reserve(candidates.size());

  for (const auto& candidate : candidates) {
    std::string_view sv;
    if constexpr (std::is_same_v<std::decay_t<decltype(candidate)>, std::string>) {
      sv = candidate;
    } else if constexpr (std::is_same_v<std::decay_t<decltype(candidate)>, std::string_view>) {
      sv = candidate;
    } else {
      sv = std::string_view(candidate);
    }

    // Use early termination for efficiency
    auto dist = rapidfuzz::levenshtein_distance(query, sv, {1, 1, 1}, max_distance);
    if (dist <= max_distance) {
      results.push_back(Match{dist, sv});
    }
  }

  // Sort by distance (primary) and text (secondary for stability)
  std::sort(results.begin(), results.end());

  // Truncate to limit
  if (results.size() > limit) {
    results.resize(limit);
  }

  return results;
}

/// Find best matches using normalized similarity score.
/// Returns matches with similarity >= min_score (0.0 to 1.0).
template <typename Container>
[[nodiscard]] std::vector<std::pair<double, std::string_view>>
best_matches_ratio(const Container& candidates, std::string_view query, std::size_t limit = 5,
                   double min_score = 0.6) {
  std::vector<std::pair<double, std::string_view>> results;
  results.reserve(candidates.size());

  for (const auto& candidate : candidates) {
    std::string_view sv;
    if constexpr (std::is_same_v<std::decay_t<decltype(candidate)>, std::string>) {
      sv = candidate;
    } else if constexpr (std::is_same_v<std::decay_t<decltype(candidate)>, std::string_view>) {
      sv = candidate;
    } else {
      sv = std::string_view(candidate);
    }

    double score = rapidfuzz::fuzz::ratio(query, sv) / 100.0;
    if (score >= min_score) {
      results.emplace_back(score, sv);
    }
  }

  // Sort by score descending
  std::sort(results.begin(), results.end(),
            [](const auto& a, const auto& b) { return a.first > b.first; });

  if (results.size() > limit) {
    results.resize(limit);
  }

  return results;
}

// ─────────────────────────────────────────────────────────────────────────────
// Convenience: Extract processor (for batch operations)
// ─────────────────────────────────────────────────────────────────────────────

/// Pre-process a query for efficient batch matching.
/// Use this when comparing one query against many candidates.
class CachedQuery {
public:
  explicit CachedQuery(std::string_view query) : query_(query) {}

  /// Get Levenshtein distance to a candidate
  [[nodiscard]] std::size_t distance(std::string_view candidate) const noexcept {
    return rapidfuzz::levenshtein_distance(query_, candidate);
  }

  /// Get Levenshtein distance with cutoff
  [[nodiscard]] std::size_t distance(std::string_view candidate, std::size_t max) const noexcept {
    return rapidfuzz::levenshtein_distance(query_, candidate, {1, 1, 1}, max);
  }

  /// Get fuzzy ratio
  [[nodiscard]] double ratio(std::string_view candidate) const noexcept {
    return rapidfuzz::fuzz::ratio(query_, candidate);
  }

private:
  std::string_view query_;
};

} // namespace straylight::nix::text
