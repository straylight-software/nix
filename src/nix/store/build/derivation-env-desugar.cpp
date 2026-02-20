#include "nix/store/build/derivation-env-desugar.h"

#include "nix/store/derivation-options.h"
#include "nix/store/derivations.h"
#include "nix/store/store-api.h"

namespace nix {

std::string& DesugaredEnv::atFileEnvPair(std::string_view name, std::string file_name) {
  auto& ret = extraFiles[file_name];
  variables.insert_or_assign(std::string{name}, EnvEntry{
                                                    .prependBuildDirectory = true,
                                                    .value = std::move(file_name),
                                                });
  return ret;
}

DesugaredEnv DesugaredEnv::create(store_t& store, const derivation_t& drv,
                                  const derivation_options_t<store_path_t>& drv_options,
                                  const store_path_set_t& inputPaths) {
  DesugaredEnv res;

  if (drv.structured_attrs) {
    auto json =
        drv.structured_attrs->prepareStructuredAttrs(store, drv_options, inputPaths, drv.outputs);
    res.atFileEnvPair("NIX_ATTRS_SH_FILE", ".attrs.sh") = StructuredAttrs::writeShell(json);
    res.atFileEnvPair("NIX_ATTRS_JSON_FILE", ".attrs.json") =
        static_cast<nlohmann::json>(std::move(json)).dump();
  } else {
    /* In non-structured mode, set all bindings either directory in the
       environment or via a file, as specified by
       `derivation_options_t::passAsFile`. */
    for (auto& [envName, envValue] : drv.env) {
      if (!drv_options.passAsFile.contains(envName)) {
        res.variables.insert_or_assign(envName, EnvEntry{
                                                    .value = envValue,
                                                });
      } else {
        res.atFileEnvPair(envName + "Path",
                          ".attr-" + hash_string(hash_algorithm_t::SHA256, envName)
                                         .to_string(hash_format_t::nix32, false)) = envValue;
      }
    }

    /* Handle exportReferencesGraph(), if set. */
    for (auto& [file_name, store_paths] : drv_options.exportReferencesGraph) {
      /* Write closure info to <file_name>. */
      res.extraFiles.insert_or_assign(
          file_name, store.makeValidityRegistration(store.exportReferences(store_paths, inputPaths),
                                                    false, false));
    }
  }

  return res;
}

} // namespace nix
