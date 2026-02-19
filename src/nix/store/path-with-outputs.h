#pragma once
///@file

#include "nix/store/derived-path.h"
#include "nix/store/path.h"

namespace nix {

struct StoreDirConfig;

/**
 * This is a deprecated old type just for use by the old CLI, and older
 * versions of the RPC protocols. In new code don't use it; you want
 * `DerivedPath` instead.
 *
 * `DerivedPath` is better because it handles more cases, and does so more
 * explicitly without devious punning tricks.
 */
struct StorePathWithOutputs {
  StorePath path;
  string_set_t outputs;

  std::string to_string(const StoreDirConfig& store) const;

  DerivedPath toDerivedPath() const;

  typedef std::variant<StorePathWithOutputs, StorePath, std::monostate> ParseResult;

  static StorePathWithOutputs::ParseResult tryFromDerivedPath(const DerivedPath&);
};

std::vector<DerivedPath> to_derived_paths(const std::vector<StorePathWithOutputs>);

std::pair<std::string_view, string_set_t> parse_path_with_outputs(std::string_view s);

/**
 * Split a string specifying a derivation and a set of outputs
 * (/nix/store/hash-foo!out1,out2,...) into the derivation path
 * and the outputs.
 */
StorePathWithOutputs parse_path_with_outputs(const StoreDirConfig& store,
                                          std::string_view path_with_outputs);

class Store;

StorePathWithOutputs follow_links_to_store_path_with_outputs(const Store& store,
                                                       std::string_view path_with_outputs);

} // namespace nix
