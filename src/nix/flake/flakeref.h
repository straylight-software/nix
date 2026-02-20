#pragma once
///@file

#include <iosfwd>
#include <regex>
#include <string>
#include <tuple>
#include <utility>

#include "nix/fetchers/registry.h"
#include "nix/store/outputs-spec.h"

namespace nix {

class store_t;

namespace fetchers {
struct settings_t;
} // namespace fetchers

using FlakeId = std::string;

/**
 * A flake reference specifies how to fetch a flake or raw source
 * (e.g. from a git repository).  It is created from a URL-like syntax
 * (e.g. 'github:NixOS/patchelf'), an attrset representation (e.g. '{
 * type="github"; owner = "NixOS"; repo = "patchelf"; }'), or a local
 * path.
 *
 * Each flake will have a number of flake_ref_t objects: one for each
 * input to the flake.
 *
 * The normal method of constructing a flake_ref_t is by starting with an
 * input description (usually the attrs or a url from the flake file),
 * locating a fetcher for that input, and then capturing the input_t
 * object that fetcher generates (usually via
 * flake_ref_t::fromAttrs(attrs) or parse_flake_ref(url) calls).
 *
 * The actual fetch may not have been performed yet (i.e. a flake_ref_t may
 * be lazy), but the fetcher can be invoked at any time via the
 * flake_ref_t to ensure the store is populated with this input.
 */
struct flake_ref_t {
  /**
   * Fetcher-specific representation of the input, sufficient to
   * perform the fetch operation.
   */
  fetchers::input_t input;

  /**
   * sub-path within the fetched input that represents this input
   */
  Path subdir;

  bool operator==(const flake_ref_t& other) const = default;

  bool operator<(const flake_ref_t& other) const {
    return std::tie(input, subdir) < std::tie(other.input, other.subdir);
  }

  flake_ref_t(fetchers::input_t&& input, const Path& subdir) : input(std::move(input)), subdir(subdir) {}

  std::string to_string(bool abbreviate = false) const;

  fetchers::Attrs toAttrs() const;

  flake_ref_t resolve(const fetchers::settings_t& fetch_settings, store_t& store,
                   fetchers::UseRegistries use_registries = fetchers::UseRegistries::All) const;

  static flake_ref_t fromAttrs(const fetchers::settings_t& fetch_settings, const fetchers::Attrs& attrs);

  std::pair<ref<source_accessor_t>, flake_ref_t> lazyFetch(const fetchers::settings_t& fetch_settings,
                                                     store_t& store) const;

  /**
   * Canonicalize a flakeref for the purpose of comparing "old" and
   * "new" `original` fields in lock files.
   */
  flake_ref_t canonicalize() const;
};

std::ostream& operator<<(std::ostream& str, const flake_ref_t& flake_ref);

/**
 * @param base_dir Optional [base
 * directory](https://nix.dev/manual/nix/development/glossary.html#gloss-base-directory)
 */
flake_ref_t parse_flake_ref(const fetchers::settings_t& fetch_settings, const std::string& url,
                       const std::optional<std::filesystem::path>& base_dir = {},
                       bool allow_missing = false, bool is_flake = true,
                       bool preserve_relative_paths = false);

/**
 * @param base_dir Optional [base
 * directory](https://nix.dev/manual/nix/development/glossary.html#gloss-base-directory)
 */
std::pair<flake_ref_t, std::string>
parse_flake_ref_with_fragment(const fetchers::settings_t& fetch_settings, const std::string& url,
                          const std::optional<std::filesystem::path>& base_dir = {},
                          bool allow_missing = false, bool is_flake = true,
                          bool preserve_relative_paths = false);

/**
 * @param base_dir Optional [base
 * directory](https://nix.dev/manual/nix/development/glossary.html#gloss-base-directory)
 */
std::tuple<flake_ref_t, std::string, ExtendedOutputsSpec>
parse_flake_ref_with_fragment_and_extended_outputs_spec(
    const fetchers::settings_t& fetch_settings, const std::string& url,
    const std::optional<std::filesystem::path>& base_dir = {}, bool allow_missing = false,
    bool is_flake = true);

const static std::string flakeIdRegexS = "[a-zA-Z][a-zA-Z0-9_-]*";
extern std::regex flake_id_regex;

} // namespace nix
