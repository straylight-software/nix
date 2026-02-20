// straylight::nix::text
//
// RE2-based regex implementation with ERE (POSIX extended) compatibility.

#include "straylight/nix/text/regex.h"

#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <unordered_map>

#include <re2/re2.h>

namespace straylight::nix::text {

// ─────────────────────────────────────────────────────────────────────────────
// Error handling
// ─────────────────────────────────────────────────────────────────────────────

[[nodiscard]] std::string_view error_message(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::Ok:
      return "no error";
    case ErrorCode::BadPattern:
      return "invalid regex pattern";
    case ErrorCode::BadCharClass:
      return "invalid character class";
    case ErrorCode::BadEscape:
      return "invalid escape sequence";
    case ErrorCode::BadRepeat:
      return "invalid repetition operator";
    case ErrorCode::TooMuchMemory:
      return "pattern requires too much memory";
    case ErrorCode::InternalError:
      return "internal regex error";
  }
  return "unknown error";
}

namespace {

/// Map RE2 error codes to our error codes
ErrorCode map_re2_error(re2::RE2::ErrorCode code) {
  switch (code) {
    case re2::RE2::NoError:
      return ErrorCode::Ok;
    case re2::RE2::ErrorBadEscape:
    case re2::RE2::ErrorTrailingBackslash:
      return ErrorCode::BadEscape;
    case re2::RE2::ErrorBadCharClass:
    case re2::RE2::ErrorBadCharRange:
      return ErrorCode::BadCharClass;
    case re2::RE2::ErrorMissingBracket:
    case re2::RE2::ErrorMissingParen:
    case re2::RE2::ErrorUnexpectedParen:
    case re2::RE2::ErrorBadPerlOp:
    case re2::RE2::ErrorBadUTF8:
    case re2::RE2::ErrorBadNamedCapture:
      return ErrorCode::BadPattern;
    case re2::RE2::ErrorRepeatArgument:
    case re2::RE2::ErrorRepeatSize:
    case re2::RE2::ErrorRepeatOp:
      return ErrorCode::BadRepeat;
    case re2::RE2::ErrorPatternTooLarge:
      return ErrorCode::TooMuchMemory;
    case re2::RE2::ErrorInternal:
    default:
      return ErrorCode::InternalError;
  }
}

/// Create RE2 options for ERE (POSIX extended) mode
re2::RE2::Options make_ere_options() {
  re2::RE2::Options opts;
  // POSIX mode: leftmost-longest match semantics (required for ERE)
  opts.set_posix_syntax(true);
  opts.set_longest_match(true);
  // Allow Perl extensions that don't conflict with POSIX
  // (e.g., (?:...) for non-capturing groups)
  opts.set_perl_classes(true);
  opts.set_word_boundary(true);
  // Case sensitive by default
  opts.set_case_sensitive(true);
  // Log errors for debugging (can be disabled in production)
  opts.set_log_errors(false);
  return opts;
}

/// Create RE2 options for ECMAScript mode
re2::RE2::Options make_ecma_options() {
  re2::RE2::Options opts;
  // Default RE2 mode is similar to ECMAScript
  opts.set_posix_syntax(false);
  opts.set_longest_match(false);
  opts.set_perl_classes(true);
  opts.set_word_boundary(true);
  opts.set_case_sensitive(true);
  opts.set_log_errors(false);
  return opts;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Regex implementation
// ─────────────────────────────────────────────────────────────────────────────

std::string_view Regex::pattern() const noexcept {
  if (!impl_) {
    return {};
  }
  return impl_->pattern();
}

std::size_t Regex::num_captures() const noexcept {
  if (!impl_) {
    return 0;
  }
  return static_cast<std::size_t>(impl_->NumberOfCapturingGroups());
}

std::expected<Regex, Error> Regex::compile(std::string_view pattern) {
  auto opts = make_ere_options();
  auto impl = std::make_shared<re2::RE2>(re2::StringPiece(pattern.data(), pattern.size()), opts);

  if (!impl->ok()) {
    return std::unexpected(Error(map_re2_error(impl->error_code()), impl->error()));
  }

  return Regex(std::move(impl));
}

std::expected<Regex, Error> Regex::compile_ecma(std::string_view pattern) {
  auto opts = make_ecma_options();
  auto impl = std::make_shared<re2::RE2>(re2::StringPiece(pattern.data(), pattern.size()), opts);

  if (!impl->ok()) {
    return std::unexpected(Error(map_re2_error(impl->error_code()), impl->error()));
  }

  return Regex(std::move(impl));
}

std::optional<Match> Regex::full_match(std::string_view text) const {
  if (!impl_) {
    return std::nullopt;
  }

  const auto num_groups = static_cast<int>(impl_->NumberOfCapturingGroups()) + 1;
  std::vector<re2::StringPiece> pieces(static_cast<std::size_t>(num_groups));

  re2::StringPiece input(text.data(), text.size());
  if (!impl_->Match(input, 0, text.size(), re2::RE2::ANCHOR_BOTH, pieces.data(), num_groups)) {
    return std::nullopt;
  }

  // Convert RE2 StringPiece to our Group type
  std::vector<Group> groups;
  groups.reserve(static_cast<std::size_t>(num_groups));
  for (int i = 0; i < num_groups; ++i) {
    const auto& piece = pieces[static_cast<std::size_t>(i)];
    if (piece.data() != nullptr) {
      groups.push_back(Group{std::string_view(piece.data(), piece.size()), true});
    } else {
      // Unmatched group (e.g., alternation where this branch didn't match)
      groups.push_back(Group{{}, false});
    }
  }

  return Match(std::move(groups));
}

std::optional<Match> Regex::partial_match(std::string_view text) const {
  if (!impl_) {
    return std::nullopt;
  }

  const auto num_groups = static_cast<int>(impl_->NumberOfCapturingGroups()) + 1;
  std::vector<re2::StringPiece> pieces(static_cast<std::size_t>(num_groups));

  re2::StringPiece input(text.data(), text.size());
  if (!impl_->Match(input, 0, text.size(), re2::RE2::UNANCHORED, pieces.data(), num_groups)) {
    return std::nullopt;
  }

  // Convert RE2 StringPiece to our Group type
  std::vector<Group> groups;
  groups.reserve(static_cast<std::size_t>(num_groups));
  for (int i = 0; i < num_groups; ++i) {
    const auto& piece = pieces[static_cast<std::size_t>(i)];
    if (piece.data() != nullptr) {
      groups.push_back(Group{std::string_view(piece.data(), piece.size()), true});
    } else {
      groups.push_back(Group{{}, false});
    }
  }

  // Calculate prefix and suffix before moving groups
  std::string_view prefix_view;
  std::string_view suffix_view;
  if (!groups.empty() && groups[0].matched && pieces[0].data() != nullptr) {
    const char* match_start = pieces[0].data();
    const char* match_end = match_start + pieces[0].size();
    prefix_view =
        std::string_view(text.data(), static_cast<std::size_t>(match_start - text.data()));
    suffix_view = std::string_view(
        match_end, static_cast<std::size_t>((text.data() + text.size()) - match_end));
  }

  Match match(std::move(groups));
  match.set_context(prefix_view, suffix_view);

  return match;
}

std::vector<Match> Regex::find_all(std::string_view text) const {
  std::vector<Match> matches;
  if (!impl_) {
    return matches;
  }

  const auto num_groups = static_cast<int>(impl_->NumberOfCapturingGroups()) + 1;
  std::vector<re2::StringPiece> pieces(static_cast<std::size_t>(num_groups));

  re2::StringPiece input(text.data(), text.size());
  std::size_t pos = 0;
  const char* prev_end = text.data();

  while (pos <= text.size()) {
    if (!impl_->Match(input, static_cast<int>(pos), text.size(), re2::RE2::UNANCHORED,
                      pieces.data(), num_groups)) {
      break;
    }

    // Build groups
    std::vector<Group> groups;
    groups.reserve(static_cast<std::size_t>(num_groups));
    for (int i = 0; i < num_groups; ++i) {
      const auto& piece = pieces[static_cast<std::size_t>(i)];
      if (piece.data() != nullptr) {
        groups.push_back(Group{std::string_view(piece.data(), piece.size()), true});
      } else {
        groups.push_back(Group{{}, false});
      }
    }

    Match match(std::move(groups));

    // Calculate prefix (from end of last match to start of this match)
    const char* match_start = pieces[0].data();
    const char* match_end = match_start + pieces[0].size();
    std::string_view prefix(prev_end, static_cast<std::size_t>(match_start - prev_end));
    // Suffix will be set by next iteration or final remainder
    match.set_context(prefix, {});

    matches.push_back(std::move(match));

    // Advance position (handle zero-length matches)
    if (pieces[0].empty()) {
      pos = static_cast<std::size_t>(match_start - text.data()) + 1;
    } else {
      pos = static_cast<std::size_t>(match_end - text.data());
    }
    prev_end = match_end;
  }

  // Set suffix on last match (remaining text after all matches)
  if (!matches.empty()) {
    std::string_view suffix(prev_end,
                            static_cast<std::size_t>((text.data() + text.size()) - prev_end));
    matches.back().set_context(matches.back().prefix(), suffix);
  }

  return matches;
}

bool Regex::test(std::string_view text) const {
  if (!impl_) {
    return false;
  }
  re2::StringPiece input(text.data(), text.size());
  return impl_->Match(input, 0, text.size(), re2::RE2::ANCHOR_BOTH, nullptr, 0);
}

bool Regex::test_partial(std::string_view text) const {
  if (!impl_) {
    return false;
  }
  re2::StringPiece input(text.data(), text.size());
  return impl_->Match(input, 0, text.size(), re2::RE2::UNANCHORED, nullptr, 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// Cache implementation
// ─────────────────────────────────────────────────────────────────────────────

struct Cache::Impl {
  // Use shared_mutex for reader-writer lock pattern
  // Most accesses are reads (cache hits), writes (cache misses) are rare
  mutable std::shared_mutex mutex;
  std::unordered_map<std::string, Regex> cache;
};

Cache::Cache() : impl_(std::make_unique<Impl>()) {}

Cache::~Cache() = default;

const Regex& Cache::get(std::string_view pattern) {
  auto result = try_get(pattern);
  if (!result) {
    throw std::runtime_error(result.error().message);
  }
  return result->get();
}

std::expected<std::reference_wrapper<const Regex>, Error> Cache::try_get(std::string_view pattern) {
  // Fast path: check if already cached (shared lock)
  {
    std::shared_lock lock(impl_->mutex);
    auto it = impl_->cache.find(std::string(pattern));
    if (it != impl_->cache.end()) {
      return std::cref(it->second);
    }
  }

  // Slow path: compile and insert (exclusive lock)
  std::unique_lock lock(impl_->mutex);

  // Double-check after acquiring exclusive lock
  auto it = impl_->cache.find(std::string(pattern));
  if (it != impl_->cache.end()) {
    return std::cref(it->second);
  }

  // Compile the regex
  auto compiled = Regex::compile(pattern);
  if (!compiled) {
    return std::unexpected(compiled.error());
  }

  // Insert and return reference
  auto [inserted_it, _] = impl_->cache.emplace(std::string(pattern), std::move(*compiled));
  return std::cref(inserted_it->second);
}

void Cache::clear() {
  std::unique_lock lock(impl_->mutex);
  impl_->cache.clear();
}

std::size_t Cache::size() const noexcept {
  std::shared_lock lock(impl_->mutex);
  return impl_->cache.size();
}

} // namespace straylight::nix::text
