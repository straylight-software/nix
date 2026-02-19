#pragma once
///@file

#include "nix/util/configuration.h"

namespace nix {

enum struct EvalProfilerMode { disabled, flamegraph };

template <>
EvalProfilerMode base_setting_t<EvalProfilerMode>::parse(const std::string& str) const;

template <>
std::string base_setting_t<EvalProfilerMode>::to_string() const;

} // namespace nix
