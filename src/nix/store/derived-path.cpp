#include "nix/store/derived-path.h"

#include <optional>

#include "nix/store/derivations.h"
#include "nix/store/store-api.h"
#include "nix/util/comparator.h"
#include "nix/util/json-utils.h"

namespace nix {

// Custom implementation to avoid `ref` ptr equality
GENERATE_CMP_EXT(, std::strong_ordering, SingleDerivedPathBuilt, *me->drvPath, me->output);

// Custom implementation to avoid `ref` ptr equality

// TODO no `GENERATE_CMP_EXT` because no `std::set::operator<=>` on
// Darwin, per header.
GENERATE_EQUAL(, DerivedPathBuilt ::, DerivedPathBuilt, *me->drvPath, me->outputs);
GENERATE_ONE_CMP(, bool, DerivedPathBuilt ::, <, DerivedPathBuilt, *me->drvPath, me->outputs);

std::string DerivedPath::opaque_t::to_string(const StoreDirConfig& store) const {
  return store.printStorePath(path);
}

std::string SingleDerivedPath::Built::to_string(const StoreDirConfig& store) const {
  return drvPath->to_string(store) + "^" + output;
}

std::string SingleDerivedPath::Built::to_string_legacy(const StoreDirConfig& store) const {
  return drvPath->to_string(store) + "!" + output;
}

std::string DerivedPath::Built::to_string(const StoreDirConfig& store) const {
  return drvPath->to_string(store) + '^' + outputs.to_string();
}

std::string DerivedPath::Built::to_string_legacy(const StoreDirConfig& store) const {
  return drvPath->to_string_legacy(store) + "!" + outputs.to_string();
}

std::string SingleDerivedPath::to_string(const StoreDirConfig& store) const {
  return std::visit([&](const auto& req) { return req.to_string(store); }, raw());
}

std::string DerivedPath::to_string(const StoreDirConfig& store) const {
  return std::visit([&](const auto& req) { return req.to_string(store); }, raw());
}

std::string SingleDerivedPath::to_string_legacy(const StoreDirConfig& store) const {
  return std::visit(
      overloaded{
          [&](const SingleDerivedPath::Built& req) { return req.to_string_legacy(store); },
          [&](const SingleDerivedPath::opaque_t& req) { return req.to_string(store); },
      },
      this->raw());
}

std::string DerivedPath::to_string_legacy(const StoreDirConfig& store) const {
  return std::visit(overloaded{
                        [&](const DerivedPath::Built& req) { return req.to_string_legacy(store); },
                        [&](const DerivedPath::opaque_t& req) { return req.to_string(store); },
                    },
                    this->raw());
}

DerivedPath::opaque_t DerivedPath::opaque_t::parse(const StoreDirConfig& store, std::string_view s) {
  return {store.parseStorePath(s)};
}

void drvRequireExperiment(const SingleDerivedPath& drv,
                          const experimental_feature_settings_t& xpSettings) {
  std::visit(overloaded{
                 [&](const SingleDerivedPath::opaque_t&) {
                   // plain drv path; no experimental features required.
                 },
                 [&](const SingleDerivedPath::Built& b) {
                   xpSettings.require(xp_t::DynamicDerivations, [&] {
                     return fmt("building output '%s' of '%s'", b.output,
                                b.drvPath->getBaseStorePath().to_string());
                   });
                 },
             },
             drv.raw());
}

SingleDerivedPath::Built
SingleDerivedPath::Built::parse(const StoreDirConfig& store, ref<const SingleDerivedPath> drv,
                                OutputNameView output,
                                const experimental_feature_settings_t& xpSettings) {
  drvRequireExperiment(*drv, xpSettings);
  return {
      .drvPath = drv,
      .output = std::string{output},
  };
}

DerivedPath::Built DerivedPath::Built::parse(const StoreDirConfig& store,
                                             ref<const SingleDerivedPath> drv,
                                             OutputNameView outputsS,
                                             const experimental_feature_settings_t& xpSettings) {
  drvRequireExperiment(*drv, xpSettings);
  return {
      .drvPath = drv,
      .outputs = OutputsSpec::parse(outputsS),
  };
}

static SingleDerivedPath parseWithSingle(const StoreDirConfig& store, std::string_view s,
                                         std::string_view separator,
                                         const experimental_feature_settings_t& xpSettings) {
  size_t n = s.rfind(separator);
  return n == s.npos ? (SingleDerivedPath)SingleDerivedPath::opaque_t::parse(store, s)
                     : (SingleDerivedPath)SingleDerivedPath::Built::parse(
                           store,
                           make_ref<const SingleDerivedPath>(
                               parseWithSingle(store, s.substr(0, n), separator, xpSettings)),
                           s.substr(n + 1), xpSettings);
}

SingleDerivedPath SingleDerivedPath::parse(const StoreDirConfig& store, std::string_view s,
                                           const experimental_feature_settings_t& xpSettings) {
  return parseWithSingle(store, s, "^", xpSettings);
}

SingleDerivedPath SingleDerivedPath::parseLegacy(const StoreDirConfig& store, std::string_view s,
                                                 const experimental_feature_settings_t& xpSettings) {
  return parseWithSingle(store, s, "!", xpSettings);
}

static DerivedPath parseWith(const StoreDirConfig& store, std::string_view s,
                             std::string_view separator,
                             const experimental_feature_settings_t& xpSettings) {
  size_t n = s.rfind(separator);
  return n == s.npos ? (DerivedPath)DerivedPath::opaque_t::parse(store, s)
                     : (DerivedPath)DerivedPath::Built::parse(
                           store,
                           make_ref<const SingleDerivedPath>(
                               parseWithSingle(store, s.substr(0, n), separator, xpSettings)),
                           s.substr(n + 1), xpSettings);
}

DerivedPath DerivedPath::parse(const StoreDirConfig& store, std::string_view s,
                               const experimental_feature_settings_t& xpSettings) {
  return parseWith(store, s, "^", xpSettings);
}

DerivedPath DerivedPath::parseLegacy(const StoreDirConfig& store, std::string_view s,
                                     const experimental_feature_settings_t& xpSettings) {
  return parseWith(store, s, "!", xpSettings);
}

DerivedPath DerivedPath::fromSingle(const SingleDerivedPath& req) {
  return std::visit(overloaded{
                        [&](const SingleDerivedPath::opaque_t& o) -> DerivedPath { return o; },
                        [&](const SingleDerivedPath::Built& b) -> DerivedPath {
                          return DerivedPath::Built{
                              .drvPath = b.drvPath,
                              .outputs = OutputsSpec::Names{b.output},
                          };
                        },
                    },
                    req.raw());
}

const StorePath& SingleDerivedPath::Built::getBaseStorePath() const {
  return drvPath->getBaseStorePath();
}

const StorePath& DerivedPath::Built::getBaseStorePath() const {
  return drvPath->getBaseStorePath();
}

template <typename DP>
static inline const StorePath& getBaseStorePath_(const DP& derivedPath) {
  return std::visit(
      overloaded{
          [&](const typename DP::Built& bfd) -> auto& { return bfd.drvPath->getBaseStorePath(); },
          [&](const typename DP::opaque_t& bo) -> auto& { return bo.path; },
      },
      derivedPath.raw());
}

const StorePath& SingleDerivedPath::getBaseStorePath() const {
  return getBaseStorePath_(*this);
}

const StorePath& DerivedPath::getBaseStorePath() const {
  return getBaseStorePath_(*this);
}

} // namespace nix

namespace nlohmann {

void adl_serializer<SingleDerivedPath::opaque_t>::to_json(json& json,
                                                        const SingleDerivedPath::opaque_t& o) {
  json = o.path;
}

SingleDerivedPath::opaque_t adl_serializer<SingleDerivedPath::opaque_t>::from_json(const json& json) {
  return SingleDerivedPath::opaque_t{json};
}

void adl_serializer<SingleDerivedPath::Built>::to_json(json& json,
                                                       const SingleDerivedPath::Built& sdpb) {
  json = {
      {"drvPath", *sdpb.drvPath},
      {"output", sdpb.output},
  };
}

void adl_serializer<DerivedPath::Built>::to_json(json& json, const DerivedPath::Built& dbp) {
  json = {
      {"drvPath", *dbp.drvPath},
      {"outputs", dbp.outputs},
  };
}

SingleDerivedPath::Built
adl_serializer<SingleDerivedPath::Built>::from_json(const json& json0,
                                                    const experimental_feature_settings_t& xpSettings) {
  auto& json = getObject(json0);
  auto drvPath =
      make_ref<SingleDerivedPath>(static_cast<SingleDerivedPath>(valueAt(json, "drvPath")));
  drvRequireExperiment(*drvPath, xpSettings);
  return {
      .drvPath = std::move(drvPath),
      .output = getString(valueAt(json, "output")),
  };
}

DerivedPath::Built
adl_serializer<DerivedPath::Built>::from_json(const json& json0,
                                              const experimental_feature_settings_t& xpSettings) {
  auto& json = getObject(json0);
  auto drvPath =
      make_ref<SingleDerivedPath>(static_cast<SingleDerivedPath>(valueAt(json, "drvPath")));
  drvRequireExperiment(*drvPath, xpSettings);
  return {
      .drvPath = std::move(drvPath),
      .outputs = adl_serializer<OutputsSpec>::from_json(valueAt(json, "outputs")),
  };
}

void adl_serializer<SingleDerivedPath>::to_json(json& json, const SingleDerivedPath& sdp) {
  std::visit([&](const auto& buildable) { json = buildable; }, sdp.raw());
}

void adl_serializer<DerivedPath>::to_json(json& json, const DerivedPath& sdp) {
  std::visit([&](const auto& buildable) { json = buildable; }, sdp.raw());
}

SingleDerivedPath
adl_serializer<SingleDerivedPath>::from_json(const json& json,
                                             const experimental_feature_settings_t& xpSettings) {
  if (json.is_string())
    return static_cast<SingleDerivedPath::opaque_t>(json);
  else
    return adl_serializer<SingleDerivedPath::Built>::from_json(json, xpSettings);
}

DerivedPath adl_serializer<DerivedPath>::from_json(const json& json,
                                                   const experimental_feature_settings_t& xpSettings) {
  if (json.is_string())
    return static_cast<DerivedPath::opaque_t>(json);
  else
    return adl_serializer<DerivedPath::Built>::from_json(json, xpSettings);
}

} // namespace nlohmann
