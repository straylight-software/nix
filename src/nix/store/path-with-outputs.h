#pragma once
///@file

#include "nix/store/derived-path.h"
#include "nix/store/path.h"

namespace nix {

struct store_dir_config_t;

/**
 * This is a deprecated old type just for use by the old CLI, and older
 * versions of the RPC protocols. In new code don't use it; you want
 * `derived_path_t` instead.
 *
 * `derived_path_t` is better because it handles more cases, and does so more
 * explicitly without devious punning tricks.
 */
struct StorePathWithOutputs {
  store_path_t path;
  string_set_t outputs;

  std::string to_string(const store_dir_config_t& store) const;

  derived_path_t toDerivedPath() const;

  typedef std::variant<StorePathWithOutputs, store_path_t, std::monostate> ParseResult;

  static StorePathWithOutputs::ParseResult tryFromDerivedPath(const derived_path_t&);
};

std::vector<derived_path_t> to_derived_paths(const std::vector<StorePathWithOutputs>);

std::pair<std::string_view, string_set_t> parse_path_with_outputs(std::string_view s);

/**
 * Split a string specifying a derivation and a set of outputs
 * (/nix/store/hash-foo!out1,out2,...) into the derivation path
 * and the outputs.
 */
StorePathWithOutputs parse_path_with_outputs(const store_dir_config_t& store,
                                          std::string_view path_with_outputs);

class store_t;

StorePathWithOutputs follow_links_to_store_path_with_outputs(const store_t& store,
                                                       std::string_view path_with_outputs);

} // namespace nix
