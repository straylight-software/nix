#include "nix/store/derivation-options.h"

#include <optional>
#include <ranges>
#include <regex>
#include <string>
#include <variant>

#include "nix/store/derivations.h"
#include "nix/store/derived-path.h"
#include "nix/store/globals.h"
#include "nix/store/parsed-derivations.h"
#include "nix/store/store-api.h"
#include "nix/util/json-utils.h"
#include "nix/util/types.h"
#include "nix/util/util.h"
#include "nix/util/variant-wrapper.h"

namespace nix {

static std::optional<std::string> get_string_attr(const string_map_t& env, const StructuredAttrs* parsed,
                                                const std::string& name) {
  if (parsed) {
    auto i = parsed->structured_attrs.find(name);
    if (i == parsed->structured_attrs.end())
      return {};
    else {
      if (!i->second.is_string())
        throw Error("attribute '%s' of must be a string", name);
      return i->second.get<std::string>();
    }
  } else {
    auto i = env.find(name);
    if (i == env.end())
      return {};
    else
      return i->second;
  }
}

static bool get_bool_attr(const string_map_t& env, const StructuredAttrs* parsed,
                        const std::string& name, bool def) {
  if (parsed) {
    auto i = parsed->structured_attrs.find(name);
    if (i == parsed->structured_attrs.end())
      return def;
    else {
      if (!i->second.is_boolean())
        throw Error("attribute '%s' must be a Boolean", name);
      return i->second.get<bool>();
    }
  } else {
    auto i = env.find(name);
    if (i == env.end())
      return def;
    else
      return i->second == "1";
  }
}

static std::optional<strings_t> get_strings_attr(const string_map_t& env, const StructuredAttrs* parsed,
                                             const std::string& name) {
  if (parsed) {
    auto i = parsed->structured_attrs.find(name);
    if (i == parsed->structured_attrs.end())
      return {};
    else {
      if (!i->second.is_array())
        throw Error("attribute '%s' must be a list of strings", name);
      auto& a = get_array(i->second);
      strings_t res;
      for (auto j = a.begin(); j != a.end(); ++j) {
        if (!j->is_string())
          throw Error("attribute '%s' must be a list of strings", name);
        res.push_back(j->get<std::string>());
      }
      return res;
    }
  } else {
    auto i = env.find(name);
    if (i == env.end())
      return {};
    else
      return tokenize_string<strings_t>(i->second);
  }
}

static std::optional<string_set_t>
get_string_set_attr(const string_map_t& env, const StructuredAttrs* parsed, const std::string& name) {
  auto ss = get_strings_attr(env, parsed, name);
  return ss ? (std::optional{string_set_t{ss->begin(), ss->end()}}) : (std::optional<string_set_t>{});
}

template <typename Inputs>
using OutputChecks = derivation_options_t<Inputs>::OutputChecks;

template <typename Inputs>
using OutputChecksVariant =
    std::variant<OutputChecks<Inputs>, std::map<std::string, OutputChecks<Inputs>>>;

derivation_options_t<store_path_t>
derivation_options_from_structured_attrs(const store_dir_config_t& store, const string_map_t& env,
                                     const StructuredAttrs* parsed, bool should_warn,
                                     const experimental_feature_settings_t& mock_xp_settings) {
  /* use the SingleDerivedPath version with empty input_drvs, then
     resolve. */
  DerivedPathMap<string_set_t> empty_input_drvs{};
  auto single_derived_path_options = derivation_options_from_structured_attrs(
      store, empty_input_drvs, env, parsed, should_warn, mock_xp_settings);

  /* "Resolve" all SingleDerivedPath inputs to store_path_t. */
  auto resolved = try_resolve(single_derived_path_options,
                             [&](ref<const SingleDerivedPath> drv_path,
                                 const std::string& output_name) -> std::optional<store_path_t> {
                               // there should be nothing to resolve
                               assert(false);
                             });

  /* Since we should never need to call the call back, there should be
     no way it fails. */
  assert(resolved);

  return *resolved;
}

static void flatten(const nlohmann::json& value, string_set_t& res) {
  if (value.is_array())
    for (auto& v : value)
      flatten(v, res);
  else if (value.is_string())
    res.insert(value);
  else
    throw Error("'exportReferencesGraph' value is not an array or a string");
}

derivation_options_t<SingleDerivedPath> derivation_options_from_structured_attrs(
    const store_dir_config_t& store, const DerivedPathMap<string_set_t>& input_drvs, const string_map_t& env,
    const StructuredAttrs* parsed, bool should_warn,
    const experimental_feature_settings_t& mock_xp_settings) {
  derivation_options_t<SingleDerivedPath> defaults = {};

  std::map<std::string, SingleDerivedPath::Built> placeholders;
  if (mock_xp_settings.is_enabled(xp_t::ca_derivations)) {
    /* Initialize placeholder map from input_drvs */
    auto init_placeholders = [&](this const auto& init_placeholders,
                                ref<const SingleDerivedPath> base_path,
                                const DerivedPathMap<string_set_t>::ChildNode& node) -> void {
      for (const auto& output_name : node.value) {
        auto built = SingleDerivedPath::Built{
            .drv_path = base_path,
            .output = output_name,
        };
        placeholders.insert_or_assign(
            DownstreamPlaceholder::fromSingleDerivedPathBuilt(built, mock_xp_settings).render(),
            std::move(built));
      }

      for (const auto& [output_name, childNode] : node.childMap) {
        init_placeholders(make_ref<const SingleDerivedPath>(SingleDerivedPath::Built{
                             .drv_path = base_path,
                             .output = output_name,
                         }),
                         childNode);
      }
    };

    for (const auto& [drv_path, outputs] : input_drvs.map) {
      auto base_path = make_ref<const SingleDerivedPath>(SingleDerivedPath::opaque_t{drv_path});
      init_placeholders(base_path, outputs);
    }
  }

  auto parse_single_derived_path = [&](const std::string& path_s) -> SingleDerivedPath {
    if (auto it = placeholders.find(path_s); it != placeholders.end())
      return it->second;
    else
      return SingleDerivedPath::opaque_t{store.toStorePath(path_s).first};
  };

  auto parse_ref = [&](const std::string& path_s) -> DrvRef<SingleDerivedPath> {
    if (auto it = placeholders.find(path_s); it != placeholders.end())
      return it->second;
    if (store.isStorePath(path_s))
      return SingleDerivedPath::opaque_t{store.toStorePath(path_s).first};
    else
      return path_s;
  };

  if (should_warn && parsed) {
    auto& structured_attrs = parsed->structured_attrs;

    if (get(structured_attrs, "allowedReferences")) {
      warn("'structuredAttrs' disables the effect of the top-level attribute 'allowedReferences'; "
           "use 'outputChecks' instead");
    }
    if (get(structured_attrs, "allowedRequisites")) {
      warn("'structuredAttrs' disables the effect of the top-level attribute 'allowedRequisites'; "
           "use 'outputChecks' instead");
    }
    if (get(structured_attrs, "disallowedRequisites")) {
      warn("'structuredAttrs' disables the effect of the top-level attribute "
           "'disallowedRequisites'; use 'outputChecks' instead");
    }
    if (get(structured_attrs, "disallowedReferences")) {
      warn("'structuredAttrs' disables the effect of the top-level attribute "
           "'disallowedReferences'; use 'outputChecks' instead");
    }
    if (get(structured_attrs, "maxSize")) {
      warn("'structuredAttrs' disables the effect of the top-level attribute 'maxSize'; use "
           "'outputChecks' instead");
    }
    if (get(structured_attrs, "maxClosureSize")) {
      warn("'structuredAttrs' disables the effect of the top-level attribute 'maxClosureSize'; use "
           "'outputChecks' instead");
    }
  }

  return {
      .output_checks = [&]() -> OutputChecksVariant<SingleDerivedPath> {
        if (parsed) {
          auto& structured_attrs = parsed->structured_attrs;

          std::map<std::string, OutputChecks<SingleDerivedPath>> res;
          if (auto* output_checks = get(structured_attrs, "outputChecks")) {
            for (auto& [output_name, output_] : get_object(*output_checks)) {
              OutputChecks<SingleDerivedPath> checks;

              auto& output = get_object(output_);

              if (auto max_size = get(output, "maxSize"))
                checks.max_size = max_size->get<uint64_t>();

              if (auto maxClosureSize = get(output, "maxClosureSize"))
                checks.maxClosureSize = maxClosureSize->get<uint64_t>();

              auto get_ = [&](const std::string& name)
                  -> std::optional<std::set<DrvRef<SingleDerivedPath>>> {
                if (auto i = get(output, name)) {
                  std::set<DrvRef<SingleDerivedPath>> res;
                  for (auto j = i->begin(); j != i->end(); ++j) {
                    if (!j->is_string())
                      throw Error("attribute '%s' must be a list of strings", name);
                    res.insert(parse_ref(j->get<std::string>()));
                  }
                  return res;
                }
                return {};
              };

              res.insert_or_assign(
                  output_name,
                  OutputChecks<SingleDerivedPath>{
                      .max_size = [&]() -> std::optional<uint64_t> {
                        if (auto max_size = get(output, "maxSize"))
                          return max_size->get<uint64_t>();
                        else
                          return std::nullopt;
                      }(),
                      .maxClosureSize = [&]() -> std::optional<uint64_t> {
                        if (auto maxClosureSize = get(output, "maxClosureSize"))
                          return maxClosureSize->get<uint64_t>();
                        else
                          return std::nullopt;
                      }(),
                      .allowedReferences = get_("allowedReferences"),
                      .disallowedReferences = get_("disallowedReferences")
                                                  .value_or(std::set<DrvRef<SingleDerivedPath>>{}),
                      .allowedRequisites = get_("allowedRequisites"),
                      .disallowedRequisites = get_("disallowedRequisites")
                                                  .value_or(std::set<DrvRef<SingleDerivedPath>>{}),
                  });
            }
          }
          return res;
        } else {
          auto parseRefSet = [&](const std::optional<string_set_t> optionalStringSet)
              -> std::optional<std::set<DrvRef<SingleDerivedPath>>> {
            if (!optionalStringSet)
              return std::nullopt;
            auto range = *optionalStringSet | std::views::transform(parse_ref);
            return std::set<DrvRef<SingleDerivedPath>>(range.begin(), range.end());
          };
          return OutputChecks<SingleDerivedPath>{
              // legacy non-structured-attributes case
              .ignoreSelfRefs = true,
              .allowedReferences = parseRefSet(get_string_set_attr(env, parsed, "allowedReferences")),
              .disallowedReferences =
                  parseRefSet(get_string_set_attr(env, parsed, "disallowedReferences"))
                      .value_or(std::set<DrvRef<SingleDerivedPath>>{}),
              .allowedRequisites = parseRefSet(get_string_set_attr(env, parsed, "allowedRequisites")),
              .disallowedRequisites =
                  parseRefSet(get_string_set_attr(env, parsed, "disallowedRequisites"))
                      .value_or(std::set<DrvRef<SingleDerivedPath>>{}),
          };
        }
      }(),
      .unsafeDiscardReferences =
          [&] {
            std::map<std::string, bool> res;

            if (parsed) {
              auto& structured_attrs = parsed->structured_attrs;

              if (auto* udr = get(structured_attrs, "unsafeDiscardReferences")) {
                for (auto& [output_name, output] : get_object(*udr)) {
                  if (!output.is_boolean())
                    throw Error("attribute 'unsafeDiscardReferences.\"%s\"' must be a Boolean",
                                output_name);
                  res.insert_or_assign(output_name, output.get<bool>());
                }
              }
            }

            return res;
          }(),
      .passAsFile =
          [&] {
            string_set_t res;
            if (auto* passAsFileString = get(env, "passAsFile")) {
              if (parsed) {
                if (should_warn) {
                  warn("'structuredAttrs' disables the effect of the top-level attribute "
                       "'passAsFile'; because all JSON is always passed via file");
                }
              } else {
                res = tokenize_string<string_set_t>(*passAsFileString);
              }
            }
            return res;
          }(),
      .exportReferencesGraph =
          [&] {
            std::map<std::string, std::set<SingleDerivedPath>> ret;

            if (parsed) {
              auto* e = optional_value_at(parsed->structured_attrs, "exportReferencesGraph");
              if (!e || !e->is_object())
                return ret;
              for (auto& [key, storePathsJson] : get_object(*e)) {
                string_set_t ss;
                flatten(storePathsJson, ss);
                std::set<SingleDerivedPath> store_paths;
                for (auto& s : ss)
                  store_paths.insert(parse_single_derived_path(s));
                ret.insert_or_assign(key, std::move(store_paths));
              }
            } else {
              auto s = get_or(env, "exportReferencesGraph", "");
              strings_t ss = tokenize_string<strings_t>(s);
              if (ss.size() % 2 != 0)
                throw Error("odd number of tokens in 'exportReferencesGraph': '%1%'", s);
              for (strings_t::iterator i = ss.begin(); i != ss.end();) {
                auto file_name = std::move(*i++);
                static std::regex regex("[A-Za-z_][A-Za-z0-9_.-]*");
                if (!std::regex_match(file_name, regex))
                  throw Error("invalid file name '%s' in 'exportReferencesGraph'", file_name);

                auto& store_path_s = *i++;
                ret.insert_or_assign(std::move(file_name),
                                     std::set{parse_single_derived_path(store_path_s)});
              }
            }
            return ret;
          }(),
      .additionalSandboxProfile = get_string_attr(env, parsed, "__sandboxProfile")
                                      .value_or(defaults.additionalSandboxProfile),
      .noChroot = get_bool_attr(env, parsed, "__noChroot", defaults.noChroot),
      .impureHostDeps =
          get_string_set_attr(env, parsed, "__impureHostDeps").value_or(defaults.impureHostDeps),
      .impureEnvVars =
          get_string_set_attr(env, parsed, "impureEnvVars").value_or(defaults.impureEnvVars),
      .allowLocalNetworking =
          get_bool_attr(env, parsed, "__darwinAllowLocalNetworking", defaults.allowLocalNetworking),
      .requiredSystemFeatures = get_string_set_attr(env, parsed, "requiredSystemFeatures")
                                    .value_or(defaults.requiredSystemFeatures),
      .preferLocalBuild = get_bool_attr(env, parsed, "preferLocalBuild", defaults.preferLocalBuild),
      .allowSubstitutes = get_bool_attr(env, parsed, "allowSubstitutes", defaults.allowSubstitutes),
  };
}

template <typename input_t>
string_set_t derivation_options_t<input_t>::getRequiredSystemFeatures(const basic_derivation_t& drv) const {
  // FIXME: cache this?
  string_set_t res;
  for (auto& i : requiredSystemFeatures)
    res.insert(i);
  if (!drv.type().hasKnownOutputPaths())
    res.insert("ca-derivations");
  return res;
}

template <typename input_t>
bool derivation_options_t<input_t>::canBuildLocally(store_t& localStore,
                                               const basic_derivation_t& drv) const {
  if (drv.platform != settings.thisSystem.get() && drv.platform != "wasm32-wasip1" &&
      !settings.extraPlatforms.get().count(drv.platform) && !drv.isBuiltin())
    return false;

  if (settings.max_build_jobs.get() == 0 && !drv.isBuiltin())
    return false;

  for (auto& feature : getRequiredSystemFeatures(drv))
    if (!localStore.config.systemFeatures.get().count(feature))
      return false;

  return true;
}

template <typename input_t>
bool derivation_options_t<input_t>::willBuildLocally(store_t& localStore,
                                                const basic_derivation_t& drv) const {
  return preferLocalBuild && canBuildLocally(localStore, drv);
}

template <typename input_t>
bool derivation_options_t<input_t>::substitutesAllowed() const {
  return settings.alwaysAllowSubstitutes ? true : allowSubstitutes;
}

template <typename input_t>
bool derivation_options_t<input_t>::useUidRange(const basic_derivation_t& drv) const {
  return getRequiredSystemFeatures(drv).count("uid-range");
}

std::optional<derivation_options_t<store_path_t>>
try_resolve(const derivation_options_t<SingleDerivedPath>& drv_options,
           std::function<std::optional<store_path_t>(ref<const SingleDerivedPath> drv_path,
                                                  const std::string& output_name)>
               queryResolutionChain) {
  auto try_resolve_path = [&](const SingleDerivedPath& input) -> std::optional<store_path_t> {
    return std::visit(
        overloaded{
            [](const SingleDerivedPath::opaque_t& p) -> std::optional<store_path_t> { return p.path; },
            [&](const SingleDerivedPath::Built& p) -> std::optional<store_path_t> {
              return queryResolutionChain(p.drv_path, p.output);
            }},
        input.raw());
  };

  auto try_resolve_ref =
      [&](const DrvRef<SingleDerivedPath>& ref) -> std::optional<DrvRef<store_path_t>> {
    return std::visit(
        overloaded{[](const OutputName& output_name) -> std::optional<DrvRef<store_path_t>> {
                     return output_name;
                   },
                   [&](const SingleDerivedPath& input) -> std::optional<DrvRef<store_path_t>> {
                     return try_resolve_path(input);
                   }},
        ref);
  };

  auto try_resolve_ref_set = [&](const std::set<DrvRef<SingleDerivedPath>>& refSet)
      -> std::optional<std::set<DrvRef<store_path_t>>> {
    std::set<DrvRef<store_path_t>> resolvedSet;
    for (const auto& ref : refSet) {
      auto resolved_ref = try_resolve_ref(ref);
      if (!resolved_ref)
        return std::nullopt;
      resolvedSet.insert(*resolved_ref);
    }
    return resolvedSet;
  };

  // Helper function to try resolving OutputChecks using functional style
  auto try_resolve_output_checks =
      [&](const derivation_options_t<SingleDerivedPath>::OutputChecks& checks)
      -> std::optional<derivation_options_t<store_path_t>::OutputChecks> {
    std::optional<std::set<DrvRef<store_path_t>>> resolvedAllowedReferences;
    if (checks.allowedReferences) {
      resolvedAllowedReferences = try_resolve_ref_set(*checks.allowedReferences);
      if (!resolvedAllowedReferences)
        return std::nullopt;
    }

    std::optional<std::set<DrvRef<store_path_t>>> resolvedAllowedRequisites;
    if (checks.allowedRequisites) {
      resolvedAllowedRequisites = try_resolve_ref_set(*checks.allowedRequisites);
      if (!resolvedAllowedRequisites)
        return std::nullopt;
    }

    auto resolvedDisallowedReferences = try_resolve_ref_set(checks.disallowedReferences);
    if (!resolvedDisallowedReferences)
      return std::nullopt;

    auto resolvedDisallowedRequisites = try_resolve_ref_set(checks.disallowedRequisites);
    if (!resolvedDisallowedRequisites)
      return std::nullopt;

    return derivation_options_t<store_path_t>::OutputChecks{
        .ignoreSelfRefs = checks.ignoreSelfRefs,
        .max_size = checks.max_size,
        .maxClosureSize = checks.maxClosureSize,
        .allowedReferences = resolvedAllowedReferences,
        .disallowedReferences = *resolvedDisallowedReferences,
        .allowedRequisites = resolvedAllowedRequisites,
        .disallowedRequisites = *resolvedDisallowedRequisites,
    };
  };

  // Helper function to resolve exportReferencesGraph using functional style
  auto try_resolve_export_references_graph =
      [&](const std::map<std::string, std::set<SingleDerivedPath>>& exportGraph)
      -> std::optional<std::map<std::string, std::set<store_path_t>>> {
    std::map<std::string, std::set<store_path_t>> resolved;
    for (const auto& [name, inputPaths] : exportGraph) {
      std::set<store_path_t> resolvedPaths;
      for (const auto& inputPath : inputPaths) {
        auto resolvedPath = try_resolve_path(inputPath);
        if (!resolvedPath)
          return std::nullopt;
        resolvedPaths.insert(*resolvedPath);
      }
      resolved.emplace(name, std::move(resolvedPaths));
    }
    return resolved;
  };

  // Resolve outputChecks using functional style with std::visit
  auto resolved_output_checks = std::visit(
      overloaded{
          [&](const derivation_options_t<SingleDerivedPath>::OutputChecks& checks)
              -> std::optional<
                  std::variant<derivation_options_t<store_path_t>::OutputChecks,
                               std::map<std::string, derivation_options_t<store_path_t>::OutputChecks>>> {
            auto resolved = try_resolve_output_checks(checks);
            if (!resolved)
              return std::nullopt;
            return std::variant<derivation_options_t<store_path_t>::OutputChecks,
                                std::map<std::string, derivation_options_t<store_path_t>::OutputChecks>>(
                *resolved);
          },
          [&](const std::map<std::string, derivation_options_t<SingleDerivedPath>::OutputChecks>&
                  checksMap)
              -> std::optional<
                  std::variant<derivation_options_t<store_path_t>::OutputChecks,
                               std::map<std::string, derivation_options_t<store_path_t>::OutputChecks>>> {
            std::map<std::string, derivation_options_t<store_path_t>::OutputChecks> resolvedMap;
            for (const auto& [output_name, checks] : checksMap) {
              auto resolved = try_resolve_output_checks(checks);
              if (!resolved)
                return std::nullopt;
              resolvedMap.emplace(output_name, *resolved);
            }
            return std::variant<derivation_options_t<store_path_t>::OutputChecks,
                                std::map<std::string, derivation_options_t<store_path_t>::OutputChecks>>(
                resolvedMap);
          }},
      drv_options.output_checks);

  if (!resolved_output_checks)
    return std::nullopt;

  // Resolve exportReferencesGraph
  auto resolved_export_graph = try_resolve_export_references_graph(drv_options.exportReferencesGraph);
  if (!resolved_export_graph)
    return std::nullopt;

  // Return resolved derivation_options_t using designated initializers
  return derivation_options_t<store_path_t>{
      .output_checks = *resolved_output_checks,
      .unsafeDiscardReferences = drv_options.unsafeDiscardReferences,
      .passAsFile = drv_options.passAsFile,
      .exportReferencesGraph = *resolved_export_graph,
      .additionalSandboxProfile = drv_options.additionalSandboxProfile,
      .noChroot = drv_options.noChroot,
      .impureHostDeps = drv_options.impureHostDeps,
      .impureEnvVars = drv_options.impureEnvVars,
      .allowLocalNetworking = drv_options.allowLocalNetworking,
      .requiredSystemFeatures = drv_options.requiredSystemFeatures,
      .preferLocalBuild = drv_options.preferLocalBuild,
      .allowSubstitutes = drv_options.allowSubstitutes,
  };
}

template struct derivation_options_t<store_path_t>;
template struct derivation_options_t<SingleDerivedPath>;

} // namespace nix

namespace nlohmann {

using namespace nix;

derivation_options_t<SingleDerivedPath>
adl_serializer<derivation_options_t<SingleDerivedPath>>::from_json(const json& json_) {
  auto& json = get_object(json_);

  return {
      .output_checks = [&]() -> OutputChecksVariant<SingleDerivedPath> {
        auto output_checks = get_object(value_at(json, "outputChecks"));

        auto forAllOutputsOpt = optional_value_at(output_checks, "forAllOutputs");
        auto perOutputOpt = optional_value_at(output_checks, "perOutput");

        if (forAllOutputsOpt && !perOutputOpt) {
          return static_cast<OutputChecks<SingleDerivedPath>>(*forAllOutputsOpt);
        } else if (perOutputOpt && !forAllOutputsOpt) {
          return static_cast<std::map<std::string, OutputChecks<SingleDerivedPath>>>(*perOutputOpt);
        } else {
          throw Error("Exactly one of 'perOutput' or 'forAllOutputs' is required");
        }
      }(),

      .unsafeDiscardReferences = value_at(json, "unsafeDiscardReferences"),
      .passAsFile = get_string_set(value_at(json, "passAsFile")),
      .exportReferencesGraph = value_at(json, "exportReferencesGraph"),

      .additionalSandboxProfile = get_string(value_at(json, "additionalSandboxProfile")),
      .noChroot = get_boolean(value_at(json, "noChroot")),
      .impureHostDeps = get_string_set(value_at(json, "impureHostDeps")),
      .impureEnvVars = get_string_set(value_at(json, "impureEnvVars")),
      .allowLocalNetworking = get_boolean(value_at(json, "allowLocalNetworking")),

      .requiredSystemFeatures = get_string_set(value_at(json, "requiredSystemFeatures")),
      .preferLocalBuild = get_boolean(value_at(json, "preferLocalBuild")),
      .allowSubstitutes = get_boolean(value_at(json, "allowSubstitutes")),
  };
}

void adl_serializer<derivation_options_t<SingleDerivedPath>>::to_json(
    json& json, const derivation_options_t<SingleDerivedPath>& o) {
  json["outputChecks"] = std::visit(
      overloaded{
          [&](const OutputChecks<SingleDerivedPath>& checks) {
            nlohmann::json output_checks;
            output_checks["forAllOutputs"] = checks;
            return output_checks;
          },
          [&](const std::map<std::string, OutputChecks<SingleDerivedPath>>& checksPerOutput) {
            nlohmann::json output_checks;
            output_checks["perOutput"] = checksPerOutput;
            return output_checks;
          },
      },
      o.output_checks);

  json["unsafeDiscardReferences"] = o.unsafeDiscardReferences;
  json["passAsFile"] = o.passAsFile;
  json["exportReferencesGraph"] = o.exportReferencesGraph;

  json["additionalSandboxProfile"] = o.additionalSandboxProfile;
  json["noChroot"] = o.noChroot;
  json["impureHostDeps"] = o.impureHostDeps;
  json["impureEnvVars"] = o.impureEnvVars;
  json["allowLocalNetworking"] = o.allowLocalNetworking;

  json["requiredSystemFeatures"] = o.requiredSystemFeatures;
  json["preferLocalBuild"] = o.preferLocalBuild;
  json["allowSubstitutes"] = o.allowSubstitutes;
}

OutputChecks<SingleDerivedPath>
adl_serializer<OutputChecks<SingleDerivedPath>>::from_json(const json& json_) {
  auto& json = get_object(json_);

  return {
      .ignoreSelfRefs = get_boolean(value_at(json, "ignoreSelfRefs")),
      .max_size = ptr_to_owned<uint64_t>(get_nullable(value_at(json, "maxSize"))),
      .maxClosureSize = ptr_to_owned<uint64_t>(get_nullable(value_at(json, "maxClosureSize"))),
      .allowedReferences = ptr_to_owned<std::set<DrvRef<SingleDerivedPath>>>(
          get_nullable(value_at(json, "allowedReferences"))),
      .disallowedReferences = value_at(json, "disallowedReferences"),
      .allowedRequisites = ptr_to_owned<std::set<DrvRef<SingleDerivedPath>>>(
          get_nullable(value_at(json, "allowedRequisites"))),
      .disallowedRequisites = value_at(json, "disallowedRequisites"),
  };
}

void adl_serializer<OutputChecks<SingleDerivedPath>>::to_json(
    json& json, const OutputChecks<SingleDerivedPath>& c) {
  json["ignoreSelfRefs"] = c.ignoreSelfRefs;
  json["maxSize"] = c.max_size;
  json["maxClosureSize"] = c.maxClosureSize;
  json["allowedReferences"] = c.allowedReferences;
  json["disallowedReferences"] = c.disallowedReferences;
  json["allowedRequisites"] = c.allowedRequisites;
  json["disallowedRequisites"] = c.disallowedRequisites;
}

} // namespace nlohmann
