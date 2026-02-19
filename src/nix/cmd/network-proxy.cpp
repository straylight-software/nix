#include "nix/cmd/network-proxy.h"

#include <algorithm>

#include "nix/util/environment-variables.h"

namespace nix {

static const string_set_t lowercaseVariables{"http_proxy", "https_proxy", "ftp_proxy", "all_proxy",
                                          "no_proxy"};

static string_set_t getAllVariables() {
  string_set_t variables = lowercaseVariables;
  for (const auto& variable : lowercaseVariables) {
    std::string upperVariable;
    std::transform(variable.begin(), variable.end(), upperVariable.begin(),
                   [](unsigned char c) { return std::toupper(c); });
    variables.insert(std::move(upperVariable));
  }
  return variables;
}

const string_set_t networkProxyVariables = getAllVariables();

static string_set_t getExcludingNoProxyVariables() {
  static const string_set_t excludeVariables{"no_proxy", "NO_PROXY"};
  string_set_t variables;
  std::set_difference(networkProxyVariables.begin(), networkProxyVariables.end(),
                      excludeVariables.begin(), excludeVariables.end(),
                      std::inserter(variables, variables.begin()));
  return variables;
}

static const string_set_t excludingNoProxyVariables = getExcludingNoProxyVariables();

bool haveNetworkProxyConnection() {
  for (const auto& variable : excludingNoProxyVariables) {
    if (getEnv(variable).has_value()) {
      return true;
    }
  }
  return false;
}

} // namespace nix
