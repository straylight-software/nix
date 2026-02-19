#include "nix/store/path-with-outputs.h"

#include <regex>

#include "nix/store/store-api.h"
#include "nix/util/strings.h"

namespace nix {

std::string StorePathWithOutputs::to_string(const StoreDirConfig& store) const {
  return outputs.empty() ? store.printStorePath(path)
                         : store.printStorePath(path) + "!" + concat_strings_sep(",", outputs);
}

DerivedPath StorePathWithOutputs::toDerivedPath() const {
  if (!outputs.empty()) {
    return DerivedPath::Built{
        .drv_path = makeConstantStorePathRef(path),
        .outputs = OutputsSpec::Names{outputs},
    };
  } else if (path.is_derivation()) {
    assert(outputs.empty());
    return DerivedPath::Built{
        .drv_path = makeConstantStorePathRef(path),
        .outputs = OutputsSpec::All{},
    };
  } else {
    return DerivedPath::opaque_t{path};
  }
}

std::vector<DerivedPath> to_derived_paths(const std::vector<StorePathWithOutputs> ss) {
  std::vector<DerivedPath> reqs;
  reqs.reserve(ss.size());
  for (auto& s : ss)
    reqs.push_back(s.toDerivedPath());
  return reqs;
}

StorePathWithOutputs::ParseResult StorePathWithOutputs::tryFromDerivedPath(const DerivedPath& p) {
  return std::visit(
      overloaded{
          [&](const DerivedPath::opaque_t& bo) -> StorePathWithOutputs::ParseResult {
            if (bo.path.is_derivation()) {
              // drv path gets interpreted as "build", not "get drv file itself"
              return bo.path;
            }
            return StorePathWithOutputs{bo.path};
          },
          [&](const DerivedPath::Built& bfd) -> StorePathWithOutputs::ParseResult {
            return std::visit(
                overloaded{
                    [&](const SingleDerivedPath::opaque_t& bo) -> StorePathWithOutputs::ParseResult {
                      return StorePathWithOutputs{
                          .path = bo.path,
                          // Use legacy encoding of wildcard as empty set
                          .outputs = std::visit(
                              overloaded{
                                  [&](const OutputsSpec::All&) -> string_set_t { return {}; },
                                  [&](const OutputsSpec::Names& outputs) {
                                    return static_cast<string_set_t>(outputs);
                                  },
                              },
                              bfd.outputs.raw),
                      };
                    },
                    [&](const SingleDerivedPath::Built&) -> StorePathWithOutputs::ParseResult {
                      return std::monostate{};
                    },
                },
                bfd.drv_path->raw());
          },
      },
      p.raw());
}

std::pair<std::string_view, string_set_t> parse_path_with_outputs(std::string_view s) {
  size_t n = s.find("!");
  return n == s.npos
             ? std::make_pair(s, string_set_t())
             : std::make_pair(s.substr(0, n), tokenize_string<string_set_t>(s.substr(n + 1), ","));
}

StorePathWithOutputs parse_path_with_outputs(const StoreDirConfig& store,
                                          std::string_view path_with_outputs) {
  auto [path, outputs] = parse_path_with_outputs(path_with_outputs);
  return StorePathWithOutputs{store.parseStorePath(path), std::move(outputs)};
}

StorePathWithOutputs follow_links_to_store_path_with_outputs(const Store& store,
                                                       std::string_view path_with_outputs) {
  auto [path, outputs] = parse_path_with_outputs(path_with_outputs);
  return StorePathWithOutputs{store.followLinksToStorePath(path), std::move(outputs)};
}

} // namespace nix
