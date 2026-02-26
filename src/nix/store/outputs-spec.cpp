#include "nix/store/outputs-spec.h"

#include <string_view>

#include <nlohmann/json.hpp>

#include "nix/store/path.h"
#include "nix/store/store-dir-config.h"
#include "nix/util/strings-inline.h"
#include "nix/util/util.h"

namespace nix {

bool OutputsSpec::contains(const std::string& output_name) const {
  return std::visit(
      overloaded{
          [&](const OutputsSpec::All&) { return true; },
          [&](const OutputsSpec::Names& outputNames) { return outputNames.count(output_name) > 0; },
      },
      raw);
}

std::optional<OutputsSpec> OutputsSpec::parseOpt(std::string_view s) {
  try {
    return parse(s);
  } catch (BadStorePathName&) {
    return std::nullopt;
  }
}

OutputsSpec OutputsSpec::parse(std::string_view s) {
  using namespace std::string_view_literals;

  if (s == "*"sv)
    return OutputsSpec::All{};

  auto names = split_string<string_set_t>(s, ",");
  for (const auto& name : names)
    check_name(name);

  return OutputsSpec::Names{std::move(names)};
}

std::optional<std::pair<std::string_view, ExtendedOutputsSpec>>
ExtendedOutputsSpec::parseOpt(std::string_view s) {
  auto found = s.rfind('^');

  if (found == std::string::npos)
    return std::pair{s, ExtendedOutputsSpec::Default{}};

  auto specOpt = OutputsSpec::parseOpt(s.substr(found + 1));
  if (!specOpt)
    return std::nullopt;
  return std::pair{s.substr(0, found), ExtendedOutputsSpec::explicit_t{std::move(*specOpt)}};
}

std::pair<std::string_view, ExtendedOutputsSpec> ExtendedOutputsSpec::parse(std::string_view s) {
  std::optional spec = parseOpt(s);
  if (!spec)
    throw Error("invalid extended outputs specifier '%s'", s);
  return *spec;
}

std::string OutputsSpec::to_string() const {
  return std::visit(overloaded{
                        [&](const OutputsSpec::All&) -> std::string { return "*"; },
                        [&](const OutputsSpec::Names& outputNames) -> std::string {
                          return concat_strings_sep(",", outputNames);
                        },
                    },
                    raw);
}

std::string ExtendedOutputsSpec::to_string() const {
  return std::visit(overloaded{
                        [&](const ExtendedOutputsSpec::Default&) -> std::string { return ""; },
                        [&](const ExtendedOutputsSpec::explicit_t& outputSpec) -> std::string {
                          return "^" + outputSpec.to_string();
                        },
                    },
                    raw);
}

OutputsSpec OutputsSpec::union_(const OutputsSpec& that) const {
  return std::visit(
      overloaded{
          [&](const OutputsSpec::All&) -> OutputsSpec { return OutputsSpec::All{}; },
          [&](const OutputsSpec::Names& theseNames) -> OutputsSpec {
            return std::visit(
                overloaded{
                    [&](const OutputsSpec::All&) -> OutputsSpec { return OutputsSpec::All{}; },
                    [&](const OutputsSpec::Names& thoseNames) -> OutputsSpec {
                      OutputsSpec::Names ret = theseNames;
                      ret.insert(thoseNames.begin(), thoseNames.end());
                      return ret;
                    },
                },
                that.raw);
          },
      },
      raw);
}

bool OutputsSpec::isSubsetOf(const OutputsSpec& that) const {
  return std::visit(overloaded{
                        [&](const OutputsSpec::All&) { return true; },
                        [&](const OutputsSpec::Names& thoseNames) {
                          return std::visit(overloaded{
                                                [&](const OutputsSpec::All&) { return false; },
                                                [&](const OutputsSpec::Names& theseNames) {
                                                  bool ret = true;
                                                  for (auto& o : theseNames)
                                                    if (thoseNames.count(o) == 0)
                                                      ret = false;
                                                  return ret;
                                                },
                                            },
                                            raw);
                        },
                    },
                    that.raw);
}

} // namespace nix

namespace nlohmann {

#ifndef DOXYGEN_SKIP

nix::OutputsSpec adl_serializer<nix::OutputsSpec>::from_json(const json& json) {
  auto names = json.get<nix::string_set_t>();
  if (names == nix::string_set_t({"*"}))
    return nix::OutputsSpec::All{};
  else
    return nix::OutputsSpec::Names{std::move(names)};
}

void adl_serializer<nix::OutputsSpec>::to_json(json& json, const nix::OutputsSpec& t) {
  std::visit(nix::overloaded{
                 [&](const nix::OutputsSpec::All&) { json = std::vector<std::string>({"*"}); },
                 [&](const nix::OutputsSpec::Names& names) { json = names; },
             },
             t.raw);
}

nix::ExtendedOutputsSpec adl_serializer<nix::ExtendedOutputsSpec>::from_json(const json& json) {
  if (json.is_null())
    return nix::ExtendedOutputsSpec::Default{};
  else {
    return nix::ExtendedOutputsSpec::explicit_t{json.get<nix::OutputsSpec>()};
  }
}

void adl_serializer<nix::ExtendedOutputsSpec>::to_json(json& json,
                                                       const nix::ExtendedOutputsSpec& t) {
  std::visit(nix::overloaded{
                 [&](const nix::ExtendedOutputsSpec::Default&) { json = nullptr; },
                 [&](const nix::ExtendedOutputsSpec::explicit_t& e) {
                   adl_serializer<nix::OutputsSpec>::to_json(json, e);
                 },
             },
             t.raw);
}

#endif

} // namespace nlohmann
