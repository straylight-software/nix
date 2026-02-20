#pragma once
///@file

#include <set>

#include "nix/util/types.h"

namespace nix {

auto levenshtein_distance(std::string_view first, std::string_view second) -> int;

/**
 * A potential suggestion for the cli interface.
 */
class suggestion_t {
public:
  /// The smaller the better
  int distance;
  std::string suggestion;

  [[nodiscard]] auto to_string() const -> std::string;

  auto operator==(const suggestion_t&) const -> bool = default;
  auto operator<=>(const suggestion_t&) const = default;
};

class suggestions_t {
public:
  std::set<suggestion_t> suggestions;

  [[nodiscard]] auto to_string() const -> std::string;

  [[nodiscard]] auto trim(int limit = 5, int max_distance = 2) const -> suggestions_t;

  static auto best_matches(const string_set_t& all_matches, std::string_view query)
      -> suggestions_t;

  auto operator+=(const suggestions_t& other) -> suggestions_t&;
};

auto operator<<(std::ostream& str, const suggestion_t&) -> std::ostream&;
auto operator<<(std::ostream& str, const suggestions_t&) -> std::ostream&;

/**
 * Either a value of type `T`, or some suggestions
 */
template <typename T>
class or_suggestions_t {
public:
  using raw_t = std::variant<T, suggestions_t>;

  raw_t raw{};

  auto operator->() -> T* { return &**this; }

  auto operator*() -> T& { return std::get<T>(raw); }

  operator bool() const noexcept { return std::holds_alternative<T>(raw); }

  or_suggestions_t(T t) : raw(t) {}

  or_suggestions_t() : raw(suggestions_t{}) {}

  static auto failed(const suggestions_t& s) -> or_suggestions_t<T> {
    auto res = or_suggestions_t<T>();
    res.raw = s;
    return res;
  }

  static auto failed() -> or_suggestions_t<T> {
    return or_suggestions_t<T>::failed(suggestions_t{});
  }

  auto get_suggestions() -> const suggestions_t& {
    static suggestions_t const no_suggestions;
    if (const auto& suggestions = std::get_if<suggestions_t>(&raw)) {
      return *suggestions;
    }
    return no_suggestions;
  }
};

} // namespace nix
