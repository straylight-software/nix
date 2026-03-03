#pragma once
///@file

#include <string>
#include <string_view>

namespace nix::regex {

// Simple string concatenation - no stringstream overhead

static inline std::string either(std::string_view a, std::string_view b) {
  std::string result;
  result.reserve(a.size() + 1 + b.size());
  result.append(a);
  result.push_back('|');
  result.append(b);
  return result;
}

static inline std::string group(std::string_view a) {
  std::string result;
  result.reserve(a.size() + 2);
  result.push_back('(');
  result.append(a);
  result.push_back(')');
  return result;
}

static inline std::string list(std::string_view a) {
  std::string result;
  result.reserve(a.size() * 2 + 5); // "a(,a)*"
  result.append(a);
  result.append("(,");
  result.append(a);
  result.append(")*");
  return result;
}

} // namespace nix::regex
