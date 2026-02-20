#include "nix/store/store-dir-config.h"

#include "nix/store/derivations.h"
#include "nix/store/globals.h"
#include "nix/util/source-path.h"
#include "nix/util/util.h"

namespace nix {

store_path_t store_dir_config_t::parseStorePath(std::string_view path) const {
  // On Windows, `/nix/store` is not a canonical path. More broadly it
  // is unclear whether this function should be using the native
  // notion of a canonical path at all. For example, it makes to
  // support remote stores whose store dir is a non-native path (e.g.
  // Windows <-> Unix ssh-ing).
  auto p =
#ifdef _WIN32
      path
#else
      canon_path(std::string(path))
#endif
      ;
  if (dir_of(p) != store_dir)
    throw BadStorePath("path '%s' is not in the Nix store", p);
  return store_path_t(base_name_of(p));
}

std::optional<store_path_t> store_dir_config_t::maybeParseStorePath(std::string_view path) const {
  try {
    return parseStorePath(path);
  } catch (Error&) {
    return {};
  }
}

bool store_dir_config_t::isStorePath(std::string_view path) const {
  return (bool)maybeParseStorePath(path);
}

store_path_set_t store_dir_config_t::parseStorePathSet(const path_set_t& paths) const {
  store_path_set_t res;
  for (auto& i : paths)
    res.insert(parseStorePath(i));
  return res;
}

std::string store_dir_config_t::printStorePath(const store_path_t& path) const {
  return (store_dir + "/").append(path.to_string());
}

path_set_t store_dir_config_t::printStorePathSet(const store_path_set_t& paths) const {
  path_set_t res;
  for (auto& i : paths)
    res.insert(printStorePath(i));
  return res;
}

/*
The exact specification of store paths is in `protocols/store-path.md`
in the Nix manual. These few functions implement that specification.

If changes to these functions go beyond mere implementation changes i.e.
also update the user-visible behavior, please update the specification
to match.
*/

store_path_t store_dir_config_t::makeStorePath(std::string_view type, std::string_view hash,
                                               std::string_view name) const {
  /* e.g., "source:sha256:1abc...:/nix/store:foo.tar.gz" */
  auto s = std::string(type) + ":" + std::string(hash) + ":" + store_dir + ":" + std::string(name);
  auto h = compress_hash(hash_string(hash_algorithm_t::SHA256, s), 20);
  return store_path_t(h, name);
}

store_path_t store_dir_config_t::makeStorePath(std::string_view type, const Hash& hash,
                                               std::string_view name) const {
  return makeStorePath(type, hash.to_string(hash_format_t::base16, true), name);
}

store_path_t store_dir_config_t::makeOutputPath(std::string_view id, const Hash& hash,
                                                std::string_view name) const {
  return makeStorePath("output:" + std::string{id}, hash, output_path_name(name, id));
}

/* Stuff the references (if any) into the type.  This is a bit
   hacky, but we can't put them in, say, <s2> (per the grammar above)
   since that would be ambiguous. */
static std::string make_type(const store_dir_config_t& store, std::string&& type,
                             const store_references_t& references) {
  for (auto& i : references.others) {
    type += ":";
    type += store.printStorePath(i);
  }
  if (references.self)
    type += ":self";
  return std::move(type);
}

store_path_t store_dir_config_t::makeFixedOutputPath(std::string_view name,
                                                     const FixedOutputInfo& info) const {
  if (info.method == file_ingestion_method_t::git &&
      !(info.hash.algo() == hash_algorithm_t::SHA1 ||
        info.hash.algo() == hash_algorithm_t::SHA256)) {
    throw Error("Git file ingestion must use SHA-1 or SHA-256 hash, but instead using: %s",
                print_hash_algo(info.hash.algo()));
  }

  if (info.hash.algo() == hash_algorithm_t::SHA256 &&
      info.method == file_ingestion_method_t::nix_archive) {
    return makeStorePath(make_type(*this, "source", info.references), info.hash, name);
  } else {
    if (!info.references.empty()) {
      throw Error("fixed output derivation '%s' is not allowed to refer to other store paths.\nYou "
                  "may need to use the 'unsafeDiscardReferences' derivation attribute, see the "
                  "manual for more details.",
                  name);
    }
    // make a unique digest based on the parameters for creating this store object
    auto payload = "fixed:out:" + make_file_ingestion_prefix(info.method) +
                   info.hash.to_string(hash_format_t::base16, true) + ":";
    auto digest = hash_string(hash_algorithm_t::SHA256, payload);
    return makeStorePath("output:out", digest, name);
  }
}

store_path_t
store_dir_config_t::makeFixedOutputPathFromCA(std::string_view name,
                                              const ContentAddressWithReferences& ca) const {
  // New template
  return std::visit(
      overloaded{[&](const TextInfo& ti) {
                   assert(ti.hash.algo() == hash_algorithm_t::SHA256);
                   return makeStorePath(make_type(*this, "text",
                                                  store_references_t{
                                                      .others = ti.references,
                                                      .self = false,
                                                  }),
                                        ti.hash, name);
                 },
                 [&](const FixedOutputInfo& foi) { return makeFixedOutputPath(name, foi); }},
      ca.raw);
}

std::pair<store_path_t, Hash> store_dir_config_t::computeStorePath(
    std::string_view name, const source_path_t& path, content_address_method_t method,
    hash_algorithm_t hash_algo, const store_path_set_t& references, path_filter_t& filter) const {
  auto [h, size] = hash_path(path, method.getFileIngestionMethod(), hash_algo, filter);
  if (settings.warnLargePathThreshold && size && *size >= settings.warnLargePathThreshold)
    warn("hashed large path '%s' (%s)", path, render_size(*size));
  return {
      makeFixedOutputPathFromCA(name,
                                ContentAddressWithReferences::fromParts(method, h,
                                                                        {
                                                                            .others = references,
                                                                            .self = false,
                                                                        })),
      h,
  };
}

} // namespace nix
