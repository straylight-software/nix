#include "nix/store/parsed-derivations.h"

#include <regex>

#include <nlohmann/json.hpp>

#include "nix/store/derivation-options.h"
#include "nix/store/derivations.h"
#include "nix/store/store-api.h"

namespace nix {

StructuredAttrs StructuredAttrs::parse(std::string_view encoded) {
  try {
    return StructuredAttrs{
        .structured_attrs = nlohmann::json::parse(encoded),
    };
  } catch (std::exception& e) {
    throw Error("cannot process %s attribute: %s", envVarName, e.what());
  }
}

std::optional<StructuredAttrs> StructuredAttrs::tryExtract(string_pairs_t& env) {
  /* Parse the __json attribute, if any. */
  auto jsonAttr = env.find(envVarName);
  if (jsonAttr != env.end()) {
    auto encoded = std::move(jsonAttr->second);
    env.erase(jsonAttr);
    return parse(encoded);
  } else
    return {};
}

std::pair<std::string_view, std::string> StructuredAttrs::unparse() const {
  // TODO don't copy the JSON object just to dump it.
  return {envVarName, static_cast<nlohmann::json>(structured_attrs).dump()};
}

void StructuredAttrs::checkKeyNotInUse(const string_pairs_t& env) {
  if (env.count(envVarName))
    throw Error("Cannot have an environment variable named '__json'. This key is reserved for "
                "encoding structured attrs");
}

static std::regex sh_var_name("[A-Za-z_][A-Za-z0-9_]*");

/**
 * Write a JSON representation of store object metadata, such as the
 * hash and the references.
 *
 * @note Do *not* use `valid_path_info_t::to_json` because this function is
 * subject to stronger stability requirements since it is used to
 * prepare build environments. Perhaps someday we'll have a versionining
 * mechanism to allow this to evolve again and get back in sync, but for
 * now we must not change - not even extend - the behavior.
 */
static nlohmann::json path_info_to_json(store_t& store, const store_path_set_t& store_paths) {
  using nlohmann::json;

  nlohmann::json::array_t json_list = json::array();

  for (auto& store_path : store_paths) {
    auto info = store.queryPathInfo(store_path);

    auto& jsonPath = json_list.emplace_back(json::object());

    jsonPath["narHash"] = info->nar_hash.to_string(hash_format_t::nix32, true);
    jsonPath["narSize"] = info->nar_size;

    {
      auto& jsonRefs = jsonPath["references"] = json::array();
      for (auto& ref : info->references)
        jsonRefs.emplace_back(store.printStorePath(ref));
    }

    if (info->ca)
      jsonPath["ca"] = render_content_address(info->ca);

    // Add the path to the object whose metadata we are including.
    jsonPath["path"] = store.printStorePath(store_path);

    jsonPath["valid"] = true;

    jsonPath["closureSize"] = ({
      uint64_t total_nar_size = 0;
      store_path_set_t closure;
      store.computeFSClosure(info->path, closure, false, false);
      for (auto& p : closure) {
        auto info = store.queryPathInfo(p);
        total_nar_size += info->nar_size;
      }
      total_nar_size;
    });
  }
  return json_list;
}

nlohmann::json::object_t StructuredAttrs::prepareStructuredAttrs(
    store_t& store, const derivation_options_t<store_path_t>& drv_options, const store_path_set_t& inputPaths,
    const DerivationOutputs& outputs) const {
  /* Copy to then modify */
  auto json = structured_attrs;

  /* Add an "outputs" object containing the output paths. */
  nlohmann::json outputsJson;
  for (auto& i : outputs)
    outputsJson[i.first] = hash_placeholder(i.first);
  json["outputs"] = std::move(outputsJson);

  /* Handle exportReferencesGraph. */
  for (auto& [key, store_paths] : drv_options.exportReferencesGraph) {
    json[key] = path_info_to_json(store, store.exportReferences(store_paths, inputPaths));
  }

  return json;
}

std::string StructuredAttrs::writeShell(const nlohmann::json::object_t& json) {
  auto handleSimpleType = [](const nlohmann::json& value) -> std::optional<std::string> {
    if (value.is_string())
      return escape_shell_arg_always(value.get<std::string_view>());

    if (value.is_number()) {
      auto f = value.get<float>();
      if (std::ceil(f) == f)
        return std::to_string(value.get<int>());
    }

    if (value.is_null())
      return std::string("''");

    if (value.is_boolean())
      return value.get<bool>() ? std::string("1") : std::string("");

    return {};
  };

  std::string jsonSh;

  for (auto& [key, value] : json) {
    if (!std::regex_match(key, sh_var_name))
      continue;

    auto s = handleSimpleType(value);
    if (s)
      jsonSh += fmt("declare %s=%s\n", key, *s);

    else if (value.is_array()) {
      std::string s2;
      bool good = true;

      for (auto& value2 : value) {
        auto s3 = handleSimpleType(value2);
        if (!s3) {
          good = false;
          break;
        }
        s2 += *s3;
        s2 += ' ';
      }

      if (good)
        jsonSh += fmt("declare -a %s=(%s)\n", key, s2);
    }

    else if (value.is_object()) {
      std::string s2;
      bool good = true;

      for (auto& [key2, value2] : value.items()) {
        auto s3 = handleSimpleType(value2);
        if (!s3) {
          good = false;
          break;
        }
        s2 += fmt("[%s]=%s ", escape_shell_arg_always(key2), *s3);
      }

      if (good)
        jsonSh += fmt("declare -A %s=(%s)\n", key, s2);
    }
  }

  return jsonSh;
}

} // namespace nix
