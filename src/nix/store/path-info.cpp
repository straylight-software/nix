#include "nix/store/path-info.h"

#include <nlohmann/json.hpp>

#include "nix/store/store-api.h"
#include "nix/util/comparator.h"
#include "nix/util/json-utils.h"
#include "nix/util/strings.h"

namespace nix {

PathInfoJsonFormat parse_path_info_json_format(uint64_t version) {
  switch (version) {
    case 1:
      return PathInfoJsonFormat::V1;
    case 2:
      return PathInfoJsonFormat::V2;
    default:
      throw Error("unsupported path info JSON format version %d; supported versions are 1 and 2",
                  version);
  }
}

UnkeyedValidPathInfo::UnkeyedValidPathInfo(const store_dir_config_t& store, Hash nar_hash)
    : UnkeyedValidPathInfo{store.store_dir, nar_hash} {}

GENERATE_CMP_EXT(, std::weak_ordering, UnkeyedValidPathInfo, me->store_dir, me->deriver,
                 me->nar_hash, me->references, me->registrationTime, me->nar_size,
                 // me->id,
                 me->ultimate, me->sigs, me->ca);

std::string valid_path_info_t::fingerprint(const store_dir_config_t& store) const {
  if (nar_size == 0)
    throw Error("cannot calculate fingerprint of path '%s' because its size is not known",
                store.printStorePath(path));
  return "1;" + store.printStorePath(path) + ";" + nar_hash.to_string(hash_format_t::nix32, true) +
         ";" + std::to_string(nar_size) + ";" +
         concat_strings_sep(",", store.printStorePathSet(references));
}

void valid_path_info_t::sign(const store_t& store, const signer_t& signer) {
  sigs.insert(signer.sign_detached(fingerprint(store)));
}

void valid_path_info_t::sign(const store_t& store,
                             const std::vector<std::unique_ptr<signer_t>>& signers) {
  auto fingerprint = this->fingerprint(store);
  for (auto& signer : signers) {
    sigs.insert(signer->sign_detached(fingerprint));
  }
}

std::optional<ContentAddressWithReferences>
valid_path_info_t::contentAddressWithReferences() const {
  if (!ca)
    return std::nullopt;

  switch (ca->method.raw) {
    case content_address_method_t::raw_t::Text: {
      assert(references.count(path) == 0);
      return TextInfo{
          .hash = ca->hash,
          .references = references,
      };
    }

    case content_address_method_t::raw_t::flat:
    case content_address_method_t::raw_t::nix_archive:
    case content_address_method_t::raw_t::git:
    default: {
      auto refs = references;
      bool hasSelfReference = false;
      if (refs.count(path)) {
        hasSelfReference = true;
        refs.erase(path);
      }
      return FixedOutputInfo{
          .method = ca->method.getFileIngestionMethod(),
          .hash = ca->hash,
          .references =
              {
                  .others = std::move(refs),
                  .self = hasSelfReference,
              },
      };
    }
  }
}

bool valid_path_info_t::isContentAddressed(const store_dir_config_t& store) const {
  auto fullCaOpt = contentAddressWithReferences();

  if (!fullCaOpt)
    return false;

  auto caPath = store.makeFixedOutputPathFromCA(path.name(), *fullCaOpt);

  bool res = caPath == path;

  if (!res)
    printError("warning: path '%s' claims to be content-addressed but isn't",
               store.printStorePath(path));

  return res;
}

size_t valid_path_info_t::checkSignatures(const store_dir_config_t& store,
                                          const public_keys_t& public_keys) const {
  if (isContentAddressed(store))
    return maxSigs;

  size_t good = 0;
  for (auto& sig : sigs)
    if (checkSignature(store, public_keys, sig))
      good++;
  return good;
}

bool valid_path_info_t::checkSignature(const store_dir_config_t& store,
                                       const public_keys_t& public_keys,
                                       const std::string& sig) const {
  return verify_detached(fingerprint(store), sig, public_keys);
}

strings_t valid_path_info_t::shortRefs() const {
  strings_t refs;
  for (auto& r : references)
    refs.push_back(std::string(r.to_string()));
  return refs;
}

valid_path_info_t valid_path_info_t::makeFromCA(const store_dir_config_t& store,
                                                std::string_view name,
                                                ContentAddressWithReferences&& ca, Hash nar_hash) {
  valid_path_info_t res{
      store.makeFixedOutputPathFromCA(name, ca),
      UnkeyedValidPathInfo(store, nar_hash),
  };
  res.ca = content_address_t{
      .method = ca.getMethod(),
      .hash = ca.getHash(),
  };
  res.references = std::visit(overloaded{
                                  [&](TextInfo&& ti) { return std::move(ti.references); },
                                  [&](FixedOutputInfo&& foi) {
                                    auto references = std::move(foi.references.others);
                                    if (foi.references.self)
                                      references.insert(res.path);
                                    return references;
                                  },
                              },
                              std::move(ca).raw);
  return res;
}

nlohmann::json UnkeyedValidPathInfo::to_json(const store_dir_config_t* store,
                                             bool includeImpureInfo,
                                             PathInfoJsonFormat format) const {
  using nlohmann::json;

  if (format == PathInfoJsonFormat::V1)
    assert(store);

  auto json_object = json::object();

  json_object["version"] = format;

  json_object["storeDir"] = store_dir;

  json_object["narHash"] = format == PathInfoJsonFormat::V1
                               ? static_cast<json>(nar_hash.to_string(hash_format_t::sri, true))
                               : static_cast<json>(nar_hash);

  json_object["narSize"] = nar_size;

  {
    auto& jsonRefs = json_object["references"] = json::array();
    for (auto& ref : references)
      jsonRefs.emplace_back(format == PathInfoJsonFormat::V1
                                ? static_cast<json>(store->printStorePath(ref))
                                : static_cast<json>(ref));
  }

  if (format == PathInfoJsonFormat::V1)
    json_object["ca"] =
        ca ? static_cast<json>(render_content_address(*ca)) : static_cast<json>(nullptr);
  else
    json_object["ca"] = ca;

  if (includeImpureInfo) {
    if (format == PathInfoJsonFormat::V1) {
      json_object["deriver"] =
          deriver ? static_cast<json>(store->printStorePath(*deriver)) : static_cast<json>(nullptr);
    } else {
      json_object["deriver"] = deriver;
    }
    json_object["registrationTime"] =
        registrationTime ? std::optional{registrationTime} : std::nullopt;

    json_object["ultimate"] = ultimate;

    auto& sigsObj = json_object["signatures"] = json::array();
    for (auto& sig : sigs)
      sigsObj.push_back(sig);
  }

  return json_object;
}

UnkeyedValidPathInfo UnkeyedValidPathInfo::from_json(const store_dir_config_t* store,
                                                     const nlohmann::json& _json) {
  auto& json = get_object(_json);

  PathInfoJsonFormat format = PathInfoJsonFormat::V1;
  if (auto* version = optional_value_at(json, "version"))
    format = *version;

  if (format == PathInfoJsonFormat::V1)
    assert(store);

  UnkeyedValidPathInfo res{
      [&] {
        if (auto* rawStoreDir = optional_value_at(json, "storeDir"))
          return get_string(*rawStoreDir);
        else if (format == PathInfoJsonFormat::V1)
          return store->store_dir;
        else
          throw Error("'storeDir' field is required in path info JSON format version 2");
      }(),
      [&] {
        return format == PathInfoJsonFormat::V1
                   ? Hash::parse_sri(get_string(value_at(json, "narHash")))
                   : Hash(value_at(json, "narHash"));
      }(),
  };

  res.nar_size = get_unsigned(value_at(json, "narSize"));

  try {
    auto& references = get_array(value_at(json, "references"));
    for (auto& input : references)
      res.references.insert(format == PathInfoJsonFormat::V1
                                ? store->parseStorePath(get_string(input))
                                : static_cast<store_path_t>(input));
  } catch (Error& e) {
    e.add_trace({}, "while reading key 'references'");
    throw;
  }

  try {
    if (format == PathInfoJsonFormat::V1) {
      if (auto* rawCa = get_nullable(value_at(json, "ca")))
        res.ca = content_address_t::parse(get_string(*rawCa));
    } else {
      res.ca = ptr_to_owned<content_address_t>(get_nullable(value_at(json, "ca")));
    }
  } catch (Error& e) {
    e.add_trace({}, "while reading key 'ca'");
    throw;
  }

  if (auto* rawDeriver0 = optional_value_at(json, "deriver")) {
    if (format == PathInfoJsonFormat::V1) {
      if (auto* rawDeriver = get_nullable(*rawDeriver0))
        res.deriver = store->parseStorePath(get_string(*rawDeriver));
    } else {
      res.deriver = ptr_to_owned<store_path_t>(get_nullable(*rawDeriver0));
    }
  }

  if (auto* rawRegistrationTime0 = optional_value_at(json, "registrationTime"))
    if (auto* rawRegistrationTime = get_nullable(*rawRegistrationTime0))
      res.registrationTime = get_integer<time_t>(*rawRegistrationTime);

  if (auto* rawUltimate = optional_value_at(json, "ultimate"))
    res.ultimate = get_boolean(*rawUltimate);

  if (auto* rawSignatures = optional_value_at(json, "signatures"))
    res.sigs = get_string_set(*rawSignatures);

  return res;
}

} // namespace nix

namespace nlohmann {

using namespace nix;

PathInfoJsonFormat adl_serializer<PathInfoJsonFormat>::from_json(const json& json) {
  return parse_path_info_json_format(get_unsigned(json));
}

void adl_serializer<PathInfoJsonFormat>::to_json(json& json, const PathInfoJsonFormat& format) {
  json = static_cast<int>(format);
}

UnkeyedValidPathInfo adl_serializer<UnkeyedValidPathInfo>::from_json(const json& json) {
  return UnkeyedValidPathInfo::from_json(nullptr, json);
}

void adl_serializer<UnkeyedValidPathInfo>::to_json(json& json, const UnkeyedValidPathInfo& c) {
  json = c.to_json(nullptr, true, PathInfoJsonFormat::V2);
}

valid_path_info_t adl_serializer<valid_path_info_t>::from_json(const json& json0) {
  auto json = get_object(json0);

  return valid_path_info_t{
      value_at(json, "path"),
      adl_serializer<UnkeyedValidPathInfo>::from_json(json0),
  };
}

void adl_serializer<valid_path_info_t>::to_json(json& json, const valid_path_info_t& v) {
  adl_serializer<UnkeyedValidPathInfo>::to_json(json, v);
  json["path"] = v.path;
}

} // namespace nlohmann
