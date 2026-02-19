#pragma once
///@file

#include <set>

#include "nix/util/types.h"

namespace nix {

int levenshtein_distance(std::string_view first, std::string_view second);

/**
 * A potential suggestion for the cli interface.
 */
class suggestion_t {
public:
  /// The smaller the better
  int distance;
  std::string suggestion;

  std::string to_string() const;

  bool operator==(const suggestion_t&) const = default;
  auto operator<=>(const suggestion_t&) const = default;
};

class suggestions_t {
public:
  std::set<suggestion_t> suggestions;

  std::string to_string() const;

  suggestions_t trim(int limit = 5, int max_distance = 2) const;

  static suggestions_t best_matches(const string_set_t& all_matches, std::string_view query);

  suggestions_t& operator+=(const suggestions_t& other);
};

std::ostream& operator<<(std::ostream& str, const suggestion_t&);
std::ostream& operator<<(std::ostream& str, const suggestions_t&);

/**
 * Either a value of type `T`, or some suggestions
 */
template <typename T>
class or_suggestions_t {
public:
  using raw_t = std::variant<T, suggestions_t>;

  raw_t raw;

  T* operator->() { return &**this; }

  T& operator*() { return std::get<T>(raw); }

  operator bool() const noexcept { return std::holds_alternative<T>(raw); }

  or_suggestions_t(T t) : raw(t) {}

  or_suggestions_t() : raw(suggestions_t{}) {}

  static or_suggestions_t<T> failed(const suggestions_t& s) {
    auto res = or_suggestions_t<T>();
    res.raw = s;
    return res;
  }

  static or_suggestions_t<T> failed() { return or_suggestions_t<T>::failed(suggestions_t{}); }

  const suggestions_t& get_suggestions() {
    static suggestions_t no_suggestions;
    if (const auto& suggestions = std::get_if<suggestions_t>(&raw))
      return *suggestions;
    else
      return no_suggestions;
  }
};

} // namespace nix
