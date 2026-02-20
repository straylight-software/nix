#include "nix/expr/eval-profiler-settings.h"

#include <nlohmann/json.hpp>

#include "nix/util/abstract-setting-to-json.h"
#include "nix/util/config-impl.h"
#include "nix/util/configuration.h"

namespace nix {

template <>
EvalProfilerMode base_setting_t<EvalProfilerMode>::parse(const std::string& str) const {
  if (str == "disabled")
    return EvalProfilerMode::disabled;
  else if (str == "flamegraph")
    return EvalProfilerMode::flamegraph;
  else
    throw UsageError("option '%s' has invalid value '%s'", name, str);
}

template <>
struct base_setting_t<EvalProfilerMode>::trait {
  static constexpr bool appendable = false;
};

template <>
std::string base_setting_t<EvalProfilerMode>::to_string() const {
  if (value_ == EvalProfilerMode::disabled)
    return "disabled";
  else if (value_ == EvalProfilerMode::flamegraph)
    return "flamegraph";
  else
    unreachable();
}

NLOHMANN_JSON_SERIALIZE_ENUM(EvalProfilerMode, {
                                                   {EvalProfilerMode::disabled, "disabled"},
                                                   {EvalProfilerMode::flamegraph, "flamegraph"},
                                               });

/* explicit_t instantiation of templates */
template class base_setting_t<EvalProfilerMode>;

} // namespace nix
