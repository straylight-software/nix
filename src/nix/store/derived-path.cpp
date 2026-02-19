#include "nix/store/derived-path.h"

#include <optional>

#include "nix/store/derivations.h"
#include "nix/store/store-api.h"
#include "nix/util/comparator.h"
#include "nix/util/json-utils.h"

namespace nix {

// Custom implementation to avoid `ref` ptr equality
GENERATE_CMP_EXT(, std::strong_ordering, SingleDerivedPathBuilt, *me->drv_path, me->output);

// Custom implementation to avoid `ref` ptr equality

// TODO no `GENERATE_CMP_EXT` because no `std::set::operator<=>` on
// Darwin, per header.
GENERATE_EQUAL(, DerivedPathBuilt ::, DerivedPathBuilt, *me->drv_path, me->outputs);
GENERATE_ONE_CMP(, bool, DerivedPathBuilt ::, <, DerivedPathBuilt, *me->drv_path, me->outputs);

std::string DerivedPath::opaque_t::to_string(const StoreDirConfig& store) const {
  return store.printStorePath(path);
}

std::string SingleDerivedPath::Built::to_string(const StoreDirConfig& store) const {
  return drv_path->to_string(store) + "^" + output;
}

std::string SingleDerivedPath::Built::to_string_legacy(const StoreDirConfig& store) const {
  return drv_path->to_string(store) + "!" + output;
}

std::string DerivedPath::Built::to_string(const StoreDirConfig& store) const {
  return drv_path->to_string(store) + '^' + outputs.to_string();
}

std::string DerivedPath::Built::to_string_legacy(const StoreDirConfig& store) const {
  return drv_path->to_string_legacy(store) + "!" + outputs.to_string();
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

void drv_require_experiment(const SingleDerivedPath& drv,
                          const experimental_feature_settings_t& xp_settings) {
  std::visit(overloaded{
                 [&](const SingleDerivedPath::opaque_t&) {
                   // plain drv path; no experimental features required.
                 },
                 [&](const SingleDerivedPath::Built& b) {
                   xp_settings.require(xp_t::dynamic_derivations, [&] {
                     return fmt("building output '%s' of '%s'", b.output,
                                b.drv_path->getBaseStorePath().to_string());
                   });
                 },
             },
             drv.raw());
}

SingleDerivedPath::Built
SingleDerivedPath::Built::parse(const StoreDirConfig& store, ref<const SingleDerivedPath> drv,
                                OutputNameView output,
                                const experimental_feature_settings_t& xp_settings) {
  drv_require_experiment(*drv, xp_settings);
  return {
      .drv_path = drv,
      .output = std::string{output},
  };
}

DerivedPath::Built DerivedPath::Built::parse(const StoreDirConfig& store,
                                             ref<const SingleDerivedPath> drv,
                                             OutputNameView outputsS,
                                             const experimental_feature_settings_t& xp_settings) {
  drv_require_experiment(*drv, xp_settings);
  return {
      .drv_path = drv,
      .outputs = OutputsSpec::parse(outputsS),
  };
}

static SingleDerivedPath parse_with_single(const StoreDirConfig& store, std::string_view s,
                                         std::string_view separator,
                                         const experimental_feature_settings_t& xp_settings) {
  size_t n = s.rfind(separator);
  return n == s.npos ? (SingleDerivedPath)SingleDerivedPath::opaque_t::parse(store, s)
                     : (SingleDerivedPath)SingleDerivedPath::Built::parse(
                           store,
                           make_ref<const SingleDerivedPath>(
                               parse_with_single(store, s.substr(0, n), separator, xp_settings)),
                           s.substr(n + 1), xp_settings);
}

SingleDerivedPath SingleDerivedPath::parse(const StoreDirConfig& store, std::string_view s,
                                           const experimental_feature_settings_t& xp_settings) {
  return parse_with_single(store, s, "^", xp_settings);
}

SingleDerivedPath SingleDerivedPath::parseLegacy(const StoreDirConfig& store, std::string_view s,
                                                 const experimental_feature_settings_t& xp_settings) {
  return parse_with_single(store, s, "!", xp_settings);
}

static DerivedPath parse_with(const StoreDirConfig& store, std::string_view s,
                             std::string_view separator,
                             const experimental_feature_settings_t& xp_settings) {
  size_t n = s.rfind(separator);
  return n == s.npos ? (DerivedPath)DerivedPath::opaque_t::parse(store, s)
                     : (DerivedPath)DerivedPath::Built::parse(
                           store,
                           make_ref<const SingleDerivedPath>(
                               parse_with_single(store, s.substr(0, n), separator, xp_settings)),
                           s.substr(n + 1), xp_settings);
}

DerivedPath DerivedPath::parse(const StoreDirConfig& store, std::string_view s,
                               const experimental_feature_settings_t& xp_settings) {
  return parse_with(store, s, "^", xp_settings);
}

DerivedPath DerivedPath::parseLegacy(const StoreDirConfig& store, std::string_view s,
                                     const experimental_feature_settings_t& xp_settings) {
  return parse_with(store, s, "!", xp_settings);
}

DerivedPath DerivedPath::fromSingle(const SingleDerivedPath& req) {
  return std::visit(overloaded{
                        [&](const SingleDerivedPath::opaque_t& o) -> DerivedPath { return o; },
                        [&](const SingleDerivedPath::Built& b) -> DerivedPath {
                          return DerivedPath::Built{
                              .drv_path = b.drv_path,
                              .outputs = OutputsSpec::Names{b.output},
                          };
                        },
                    },
                    req.raw());
}

const StorePath& SingleDerivedPath::Built::getBaseStorePath() const {
  return drv_path->getBaseStorePath();
}

const StorePath& DerivedPath::Built::getBaseStorePath() const {
  return drv_path->getBaseStorePath();
}

template <typename DP>
static inline const StorePath& get_base_store_path_(const DP& derived_path) {
  return std::visit(
      overloaded{
          [&](const typename DP::Built& bfd) -> auto& { return bfd.drv_path->getBaseStorePath(); },
          [&](const typename DP::opaque_t& bo) -> auto& { return bo.path; },
      },
      derived_path.raw());
}

const StorePath& SingleDerivedPath::getBaseStorePath() const {
  return get_base_store_path_(*this);
}

const StorePath& DerivedPath::getBaseStorePath() const {
  return get_base_store_path_(*this);
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
      {"drvPath", *sdpb.drv_path},
      {"output", sdpb.output},
  };
}

void adl_serializer<DerivedPath::Built>::to_json(json& json, const DerivedPath::Built& dbp) {
  json = {
      {"drvPath", *dbp.drv_path},
      {"outputs", dbp.outputs},
  };
}

SingleDerivedPath::Built
adl_serializer<SingleDerivedPath::Built>::from_json(const json& json0,
                                                    const experimental_feature_settings_t& xp_settings) {
  auto& json = get_object(json0);
  auto drv_path =
      make_ref<SingleDerivedPath>(static_cast<SingleDerivedPath>(value_at(json, "drvPath")));
  drv_require_experiment(*drv_path, xp_settings);
  return {
      .drv_path = std::move(drv_path),
      .output = get_string(value_at(json, "output")),
  };
}

DerivedPath::Built
adl_serializer<DerivedPath::Built>::from_json(const json& json0,
                                              const experimental_feature_settings_t& xp_settings) {
  auto& json = get_object(json0);
  auto drv_path =
      make_ref<SingleDerivedPath>(static_cast<SingleDerivedPath>(value_at(json, "drvPath")));
  drv_require_experiment(*drv_path, xp_settings);
  return {
      .drv_path = std::move(drv_path),
      .outputs = adl_serializer<OutputsSpec>::from_json(value_at(json, "outputs")),
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
                                             const experimental_feature_settings_t& xp_settings) {
  if (json.is_string())
    return static_cast<SingleDerivedPath::opaque_t>(json);
  else
    return adl_serializer<SingleDerivedPath::Built>::from_json(json, xp_settings);
}

DerivedPath adl_serializer<DerivedPath>::from_json(const json& json,
                                                   const experimental_feature_settings_t& xp_settings) {
  if (json.is_string())
    return static_cast<DerivedPath::opaque_t>(json);
  else
    return adl_serializer<DerivedPath::Built>::from_json(json, xp_settings);
}

} // namespace nlohmann
