#include "nix/cmd/network-proxy.h"

#include <algorithm>

#include "nix/util/environment-variables.h"

namespace nix {

static const string_set_t lowercase_variables{"http_proxy", "https_proxy", "ftp_proxy", "all_proxy",
                                          "no_proxy"};

static string_set_t get_all_variables() {
  string_set_t variables = lowercase_variables;
  for (const auto& variable : lowercase_variables) {
    std::string upperVariable;
    std::transform(variable.begin(), variable.end(), upperVariable.begin(),
                   [](unsigned char c) { return std::toupper(c); });
    variables.insert(std::move(upperVariable));
  }
  return variables;
}

const string_set_t network_proxy_variables = get_all_variables();

static string_set_t get_excluding_no_proxy_variables() {
  static const string_set_t exclude_variables{"no_proxy", "NO_PROXY"};
  string_set_t variables;
  std::set_difference(network_proxy_variables.begin(), network_proxy_variables.end(),
                      exclude_variables.begin(), exclude_variables.end(),
                      std::inserter(variables, variables.begin()));
  return variables;
}

static const string_set_t excluding_no_proxy_variables = get_excluding_no_proxy_variables();

bool have_network_proxy_connection() {
  for (const auto& variable : excluding_no_proxy_variables) {
    if (get_env(variable).has_value()) {
      return true;
    }
  }
  return false;
}

} // namespace nix
