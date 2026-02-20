#include "nix/store/downstream-placeholder.h"

#include "nix/store/derivations.h"
#include "nix/util/json-utils.h"

namespace nix {

std::string DownstreamPlaceholder::render() const {
  return "/" + hash.to_string(hash_format_t::nix32, false);
}

DownstreamPlaceholder
DownstreamPlaceholder::unknownCaOutput(const store_path_t& drv_path, OutputNameView output_name,
                                       const experimental_feature_settings_t& xp_settings) {
  xp_settings.require(xp_t::ca_derivations);
  auto drvNameWithExtension = drv_path.name();
  auto drv_name = drvNameWithExtension.substr(0, drvNameWithExtension.size() - 4);
  auto clearText = "nix-upstream-output:" + std::string{drv_path.hash_part()} + ":" +
                   output_path_name(drv_name, output_name);
  return DownstreamPlaceholder{hash_string(hash_algorithm_t::SHA256, clearText)};
}

DownstreamPlaceholder
DownstreamPlaceholder::unknownDerivation(const DownstreamPlaceholder& placeholder,
                                         OutputNameView output_name,
                                         const experimental_feature_settings_t& xp_settings) {
  xp_settings.require(xp_t::dynamic_derivations, [&] {
    return fmt("placeholder for unknown derivation output '%s'", output_name);
  });
  auto compressed = compress_hash(placeholder.hash, 20);
  auto clearText = "nix-computed-output:" + compressed.to_string(hash_format_t::nix32, false) +
                   ":" + std::string{output_name};
  return DownstreamPlaceholder{hash_string(hash_algorithm_t::SHA256, clearText)};
}

DownstreamPlaceholder DownstreamPlaceholder::fromSingleDerivedPathBuilt(
    const SingleDerivedPath::Built& b, const experimental_feature_settings_t& xp_settings) {
  return std::visit(overloaded{
                        [&](const SingleDerivedPath::opaque_t& o) {
                          return DownstreamPlaceholder::unknownCaOutput(o.path, b.output,
                                                                        xp_settings);
                        },
                        [&](const SingleDerivedPath::Built& b2) {
                          return DownstreamPlaceholder::unknownDerivation(
                              DownstreamPlaceholder::fromSingleDerivedPathBuilt(b2, xp_settings),
                              b.output, xp_settings);
                        },
                    },
                    b.drv_path->raw());
}

} // namespace nix

namespace nlohmann {

using namespace nix;

template <typename Item>
DrvRef<Item> adl_serializer<DrvRef<Item>>::from_json(const json& json) {
  // OutputName case: { "drvPath": "self", "output": <output> }
  if (json.type() == nlohmann::json::value_t::object) {
    auto& obj = get_object(json);
    if (auto* drvPath_ = get(obj, "drvPath")) {
      auto& drv_path = *drvPath_;
      if (drv_path.type() == nlohmann::json::value_t::string && get_string(drv_path) == "self") {
        return get_string(value_at(obj, "output"));
      }
    }
  }

  // input_t case
  return adl_serializer<Item>::from_json(json);
}

template <typename Item>
void adl_serializer<DrvRef<Item>>::to_json(json& json, const DrvRef<Item>& ref) {
  std::visit(overloaded{
                 [&](const OutputName& output_name) {
                   json = nlohmann::json::object();
                   json["drvPath"] = "self";
                   json["output"] = output_name;
                 },
                 [&](const Item& item) { json = item; },
             },
             ref);
}

template struct adl_serializer<nix::DrvRef<store_path_t>>;
template struct adl_serializer<nix::DrvRef<SingleDerivedPath>>;

} // namespace nlohmann
