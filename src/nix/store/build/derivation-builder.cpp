#include "nix/store/build/derivation-builder.h"

#include "nix/util/json-utils.h"

namespace nlohmann {

nix::ExternalBuilder adl_serializer<nix::ExternalBuilder>::from_json(const json& json) {
  auto obj = nix::get_object(json);
  return {
      .systems = nix::value_at(obj, "systems"),
      .program = nix::value_at(obj, "program"),
      .args = nix::value_at(obj, "args"),
  };
}

void adl_serializer<nix::ExternalBuilder>::to_json(json& json, const nix::ExternalBuilder& eb) {
  json = {
      {"systems", eb.systems},
      {"program", eb.program},
      {"args", eb.args},
  };
}

} // namespace nlohmann
