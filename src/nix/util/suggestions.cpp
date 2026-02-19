#include "nix/util/suggestions.h"

#include <algorithm>
#include <sstream>

#include "nix/util/ansicolor.h"
#include "nix/util/terminal.h"

namespace nix {

int levenshtein_distance(std::string_view first, std::string_view second) {
  // Implementation borrowed from
  // https://en.wikipedia.org/wiki/Levenshtein_distance#Iterative_with_two_matrix_rows

  int m = first.size();
  int n = second.size();

  auto v0 = std::vector<int>(n + 1);
  auto v1 = std::vector<int>(n + 1);

  for (auto i = 0; i <= n; i++)
    v0[i] = i;

  for (auto i = 0; i < m; i++) {
    v1[0] = i + 1;

    for (auto j = 0; j < n; j++) {
      auto deletion_cost = v0[j + 1] + 1;
      auto insertion_cost = v1[j] + 1;
      auto substitution_cost = first[i] == second[j] ? v0[j] : v0[j] + 1;
      v1[j + 1] = std::min({deletion_cost, insertion_cost, substitution_cost});
    }

    std::swap(v0, v1);
  }

  return v0[n];
}

suggestions_t suggestions_t::best_matches(const string_set_t& all_matches, std::string_view query) {
  std::set<suggestion_t> res;
  for (const auto& possible_match : all_matches) {
    res.insert(suggestion_t{
        .distance = levenshtein_distance(query, possible_match),
        .suggestion = possible_match,
    });
  }
  return suggestions_t{res};
}

suggestions_t suggestions_t::trim(int limit, int max_distance) const {
  std::set<suggestion_t> res;

  int count = 0;

  for (auto& elt : suggestions) {
    if (count >= limit || elt.distance > max_distance)
      break;
    count++;
    res.insert(elt);
  }

  return suggestions_t{res};
}

std::string suggestion_t::to_string() const {
  return ANSI_WARNING + filter_ansi_escapes(suggestion) + ANSI_NORMAL;
}

std::string suggestions_t::to_string() const {
  switch (suggestions.size()) {
    case 0:
      return "";
    case 1:
      return suggestions.begin()->to_string();
    default: {
      std::string res = "one of ";
      auto iter = suggestions.begin();
      res += iter->to_string(); // Iter can’t be end() because the container isn’t null
      iter++;
      auto last = suggestions.end();
      last--;
      for (; iter != suggestions.end(); iter++) {
        res += (iter == last) ? " or " : ", ";
        res += iter->to_string();
      }
      return res;
    }
  }
}

suggestions_t& suggestions_t::operator+=(const suggestions_t& other) {
  suggestions.insert(other.suggestions.begin(), other.suggestions.end());
  return *this;
}

std::ostream& operator<<(std::ostream& str, const suggestion_t& suggestion) {
  return str << suggestion.to_string();
}

std::ostream& operator<<(std::ostream& str, const suggestions_t& suggestions) {
  return str << suggestions.to_string();
}

} // namespace nix
