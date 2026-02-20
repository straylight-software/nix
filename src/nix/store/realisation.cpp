#include "nix/store/realisation.h"

#include <nlohmann/json.hpp>

#include "nix/store/store-api.h"
#include "nix/util/closure.h"
#include "nix/util/json-utils.h"
#include "nix/util/signature/local-keys.h"

namespace nix {

make_error(InvalidDerivationOutputId, Error);

DrvOutput DrvOutput::parse(const std::string& strRep) {
  size_t n = strRep.find("!");
  if (n == strRep.npos)
    throw InvalidDerivationOutputId("Invalid derivation output id %s", strRep);

  return DrvOutput{
      .drvHash = Hash::parse_any_prefixed(strRep.substr(0, n)),
      .output_name = strRep.substr(n + 1),
  };
}

std::string DrvOutput::to_string() const {
  return strHash() + "!" + output_name;
}

std::set<realisation_t> realisation_t::closure(store_t& store,
                                               const std::set<realisation_t>& startOutputs) {
  std::set<realisation_t> res;
  realisation_t::closure(store, startOutputs, res);
  return res;
}

void realisation_t::closure(store_t& store, const std::set<realisation_t>& startOutputs,
                            std::set<realisation_t>& res) {
  auto getDeps = [&](const realisation_t& current) -> std::set<realisation_t> {
    std::set<realisation_t> res;
    for (auto& [currentDep, _] : current.dependentRealisations) {
      if (auto currentRealisation = store.query_realisation(currentDep))
        res.insert({*currentRealisation, currentDep});
      else
        throw Error("Unrealised derivation '%s'", currentDep.to_string());
    }
    return res;
  };

  compute_closure<realisation_t>(
      startOutputs, res,
      [&](const realisation_t& current,
          std::function<void(std::promise<std::set<realisation_t>>&)> processEdges) {
        std::promise<std::set<realisation_t>> promise;
        try {
          auto res = getDeps(current);
          promise.set_value(res);
        } catch (...) {
          promise.set_exception(std::current_exception());
        }
        return processEdges(promise);
      });
}

std::string UnkeyedRealisation::fingerprint(const DrvOutput& key) const {
  nlohmann::json serialized = realisation_t{*this, key};
  serialized.erase("signatures");
  return serialized.dump();
}

void UnkeyedRealisation::sign(const DrvOutput& key, const signer_t& signer) {
  signatures.insert(signer.sign_detached(fingerprint(key)));
}

bool UnkeyedRealisation::checkSignature(const DrvOutput& key, const public_keys_t& public_keys,
                                        const std::string& sig) const {
  return verify_detached(fingerprint(key), sig, public_keys);
}

size_t UnkeyedRealisation::checkSignatures(const DrvOutput& key,
                                           const public_keys_t& public_keys) const {
  // FIXME: Maybe we should return `maxSigs` if the realisation corresponds to
  // an input-addressed one − because in that case the drv is enough to check
  // it − but we can't know that here.

  size_t good = 0;
  for (auto& sig : signatures)
    if (checkSignature(key, public_keys, sig))
      good++;
  return good;
}

const store_path_t& RealisedPath::path() const& {
  return std::visit([](auto& arg) -> auto& { return arg.get_path(); }, raw);
}

bool realisation_t::isCompatibleWith(const UnkeyedRealisation& other) const {
  if (out_path == other.out_path) {
    if (dependentRealisations.empty() != other.dependentRealisations.empty()) {
      warn("Encountered a realisation for '%s' with an empty set of "
           "dependencies. This is likely an artifact from an older Nix. "
           "I’ll try to fix the realisation if I can",
           id.to_string());
      return true;
    } else if (dependentRealisations == other.dependentRealisations) {
      return true;
    }
  }
  return false;
}

void RealisedPath::closure(store_t& store, const RealisedPath::Set& startPaths,
                           RealisedPath::Set& ret) {
  // FIXME: This only builds the store-path closure, not the real realisation
  // closure
  store_path_set_t initialStorePaths, pathsClosure;
  for (auto& path : startPaths)
    initialStorePaths.insert(path.path());
  store.computeFSClosure(initialStorePaths, pathsClosure);
  ret.insert(startPaths.begin(), startPaths.end());
  ret.insert(pathsClosure.begin(), pathsClosure.end());
}

void RealisedPath::closure(store_t& store, RealisedPath::Set& ret) const {
  RealisedPath::closure(store, {*this}, ret);
}

RealisedPath::Set RealisedPath::closure(store_t& store) const {
  RealisedPath::Set ret;
  closure(store, ret);
  return ret;
}

} // namespace nix

namespace nlohmann {

using namespace nix;

DrvOutput adl_serializer<DrvOutput>::from_json(const json& json) {
  return DrvOutput::parse(get_string(json));
}

void adl_serializer<DrvOutput>::to_json(json& json, const DrvOutput& drvOutput) {
  json = drvOutput.to_string();
}

UnkeyedRealisation adl_serializer<UnkeyedRealisation>::from_json(const json& json0) {
  auto json = get_object(json0);

  string_set_t signatures;
  if (auto signaturesOpt = optional_value_at(json, "signatures"))
    signatures = *signaturesOpt;

  std::map<DrvOutput, store_path_t> dependentRealisations;
  if (auto jsonDependencies = optional_value_at(json, "dependentRealisations"))
    for (auto& [jsonDepId, jsonDepOutPath] : get_object(*jsonDependencies))
      dependentRealisations.insert({DrvOutput::parse(jsonDepId), jsonDepOutPath});

  return UnkeyedRealisation{
      .out_path = value_at(json, "outPath"),
      .signatures = signatures,
      .dependentRealisations = dependentRealisations,
  };
}

void adl_serializer<UnkeyedRealisation>::to_json(json& json, const UnkeyedRealisation& r) {
  auto jsonDependentRealisations = nlohmann::json::object();
  for (auto& [depId, depOutPath] : r.dependentRealisations)
    jsonDependentRealisations.emplace(depId.to_string(), depOutPath);
  json = {
      {"outPath", r.out_path},
      {"signatures", r.signatures},
      {"dependentRealisations", jsonDependentRealisations},
  };
}

realisation_t adl_serializer<realisation_t>::from_json(const json& json0) {
  auto json = get_object(json0);

  return realisation_t{
      static_cast<UnkeyedRealisation>(json0),
      value_at(json, "id"),
  };
}

void adl_serializer<realisation_t>::to_json(json& json, const realisation_t& r) {
  json = static_cast<const UnkeyedRealisation&>(r);
  json["id"] = r.id;
}

} // namespace nlohmann
