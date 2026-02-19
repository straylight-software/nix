#include "nix/store/build/derivation-builder.h"

#include "nix/util/json-utils.h"

namespace nlohmann {

using namespace nix;

ExternalBuilder adl_serializer<ExternalBuilder>::from_json(const json& json) {
  auto obj = get_object(json);
  return {
      .systems = value_at(obj, "systems"),
      .program = value_at(obj, "program"),
      .args = value_at(obj, "args"),
  };
}

void adl_serializer<ExternalBuilder>::to_json(json& json, const ExternalBuilder& eb) {
  json = {
      {"systems", eb.systems},
      {"program", eb.program},
      {"args", eb.args},
  };
}

} // namespace nlohmann
