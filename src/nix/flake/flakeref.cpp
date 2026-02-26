#include "nix/flake/flakeref.h"

#include <filesystem>
#include <optional>
#include <ostream>
#include <regex>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <assert.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "nix/fetchers/attrs.h"
#include "nix/fetchers/fetch-settings.h"
#include "nix/fetchers/fetchers.h"
#include "nix/fetchers/registry.h"
#include "nix/store/outputs-spec.h"
#include "nix/util/error.h"
#include "nix/util/file-system.h"
#include "nix/util/fmt.h"
#include "nix/util/logging.h"
#include "nix/util/ref.h"
#include "nix/util/strings.h"
#include "nix/util/types.h"
#include "nix/util/url-parts.h"
#include "nix/util/url.h"
#include "nix/util/util.h"

namespace nix {
struct store_t;
struct source_accessor_t;

namespace fetchers {
struct settings_t;
} // namespace fetchers

#if 0
// 'dir' path elements cannot start with a '.'. We also reject
// potentially dangerous characters like ';'.
const static std::string subDirElemRegex = "(?:[a-zA-Z0-9_-]+[a-zA-Z0-9._-]*)";
const static std::string subDirRegex = subDirElemRegex + "(?:/" + subDirElemRegex + ")*";
#endif

std::string flake_ref_t::to_string(bool abbreviate) const {
  string_map_t extraQuery;
  if (subdir != "")
    extraQuery.insert_or_assign("dir", subdir);
  return input.toURLString(extraQuery, abbreviate);
}

fetchers::Attrs flake_ref_t::toAttrs() const {
  auto attrs = input.toAttrs();
  if (subdir != "")
    attrs.emplace("dir", subdir);
  return attrs;
}

std::ostream& operator<<(std::ostream& str, const flake_ref_t& flake_ref) {
  str << flake_ref.to_string();
  return str;
}

flake_ref_t flake_ref_t::resolve(const fetchers::settings_t& fetch_settings, store_t& store,
                                 fetchers::UseRegistries use_registries) const {
  auto [input2, extra_attrs] = lookup_in_registries(fetch_settings, store, input, use_registries);
  return flake_ref_t(std::move(input2),
                     fetchers::maybe_get_str_attr(extra_attrs, "dir").value_or(subdir));
}

flake_ref_t parse_flake_ref(const fetchers::settings_t& fetch_settings, const std::string& url,
                            const std::optional<std::filesystem::path>& base_dir,
                            bool allow_missing, bool is_flake, bool preserve_relative_paths) {
  auto [flake_ref, fragment] = parse_flake_ref_with_fragment(
      fetch_settings, url, base_dir, allow_missing, is_flake, preserve_relative_paths);
  if (fragment != "")
    throw Error("unexpected fragment '%s' in flake reference '%s'", fragment, url);
  return flake_ref;
}

static std::pair<flake_ref_t, std::string>
from_parsed_url(const fetchers::settings_t& fetch_settings, parsed_url_t&& parsed_url,
                bool is_flake) {
  auto dir = get_or(parsed_url.query(), "dir", "");
  if (!fetch_settings.nix219Compat)
    parsed_url.query().erase("dir");

  std::string fragment = parsed_url.fragment();
  parsed_url.set_fragment("");

  return {flake_ref_t(fetchers::input_t::fromURL(fetch_settings, parsed_url, is_flake), dir),
          fragment};
}

std::pair<flake_ref_t, std::string> parse_path_flake_ref_with_fragment(
    const fetchers::settings_t& fetch_settings, const std::string& url,
    const std::optional<std::filesystem::path>& base_dir, bool allow_missing, bool is_flake,
    bool preserve_relative_paths) {
  static std::regex path_flake_regex(R"(([^?#]*)(\?([^#]*))?(#(.*))?)", std::regex::ECMAScript);

  std::smatch match;
  auto succeeds = std::regex_match(url, match, path_flake_regex);
  if (!succeeds)
    throw Error("invalid flakeref '%s'", url);
  auto path = match[1].str();
  auto query = decode_query(match[3].str(), /*lenient=*/true);
  auto fragment = percent_decode(match[5].str());

  if (base_dir) {
    /* Check if 'url' is a path (either absolute or relative
       to 'baseDir'). If so, search upward to the root of the
       repo (i.e. the directory containing .git). */

    path = abs_path(path, base_dir->string(), true);

    if (is_flake) {
      if (!S_ISDIR(lstat(path).st_mode)) {
        if (base_name_of(path) == "flake.nix") {
          // Be gentle with people who accidentally write `/foo/bar/flake.nix` instead of `/foo/bar`
          warn("Path '%s' should point at the directory containing the 'flake.nix' file, not the "
               "file itself. "
               "Pretending that you meant '%s'",
               path, dir_of(path));
          path = dir_of(path);
        } else {
          throw BadURL("path '%s' is not a flake (because it's not a directory)", path);
        }
      }

      if (!allow_missing && !path_exists(path + "/flake.nix")) {
        notice("path '%s' does not contain a 'flake.nix', searching up", path);

        // Save device to detect filesystem boundary
        dev_t device = lstat(path).st_dev;
        bool found = false;
        while (path != "/") {
          if (path_exists(path + "/flake.nix")) {
            found = true;
            break;
          } else if (path_exists(path + "/.git"))
            throw Error("path '%s' is not part of a flake (neither it nor its parent directories "
                        "contain a 'flake.nix' file)",
                        path);
          else {
            if (lstat(path).st_dev != device)
              throw Error("unable to find a flake before encountering filesystem boundary at '%s'",
                          path);
          }
          path = dir_of(path);
        }
        if (!found)
          throw BadURL("could not find a flake.nix file");
      }

      if (!allow_missing && !path_exists(path + "/flake.nix"))
        throw BadURL("path '%s' is not a flake (because it doesn't contain a 'flake.nix' file)",
                     path);

      auto flake_root = path;
      std::string subdir;

      while (flake_root != "/") {
        if (path_exists(flake_root + "/.git")) {
          parsed_url_t parsed_url;
          parsed_url.set_scheme("git+file");
          parsed_url.set_authority(parsed_url_t::authority_t{});
          parsed_url.set_path(split_string<std::vector<std::string>>(flake_root, "/"));
          parsed_url.set_query(query);
          parsed_url.set_fragment(fragment);

          if (subdir != "") {
            if (parsed_url.query().count("dir"))
              throw Error("flake URL '%s' has an inconsistent 'dir' parameter", url);
            parsed_url.query().insert_or_assign("dir", subdir);
          }

          if (path_exists(flake_root + "/.git/shallow"))
            parsed_url.query().insert_or_assign("shallow", "1");

          return from_parsed_url(fetch_settings, std::move(parsed_url), is_flake);
        }

        subdir = std::string(base_name_of(flake_root)) + (subdir.empty() ? "" : "/" + subdir);
        flake_root = dir_of(flake_root);
      }
    }

  } else {
    if (!preserve_relative_paths && !is_absolute(path))
      throw BadURL("flake reference '%s' is not an absolute path", url);
  }

  parsed_url_t path_url;
  path_url.set_scheme("path");
  path_url.set_authority(parsed_url_t::authority_t{});
  path_url.set_path(split_string<std::vector<std::string>>(path, "/"));
  path_url.set_query(query);
  path_url.set_fragment(fragment);
  return from_parsed_url(fetch_settings, std::move(path_url), is_flake);
}

/**
 * Check if `url` is a flake ID. This is an abbreviated syntax for
 * `flake:<flake-id>?ref=<ref>&rev=<rev>`.
 */
static std::optional<std::pair<flake_ref_t, std::string>>
parseFlakeIdRef(const fetchers::settings_t& fetch_settings, const std::string& url, bool is_flake) {
  std::smatch match;

  static std::regex flake_regex("((" + flakeIdRegexS + ")(?:/(?:" + ref_and_or_rev_regex + "))?)" +
                                    "(?:#(" + fragment_regex + "))?",
                                std::regex::ECMAScript);

  if (std::regex_match(url, match, flake_regex)) {
    parsed_url_t parsed_url;
    parsed_url.set_scheme("flake");
    parsed_url.set_authority(std::nullopt);
    parsed_url.set_path(split_string<std::vector<std::string>>(match[1].str(), "/"));

    return std::make_pair(
        flake_ref_t(fetchers::input_t::fromURL(fetch_settings, parsed_url, is_flake), ""),
        percent_decode(match.str(6)));
  }

  return {};
}

std::optional<std::pair<flake_ref_t, std::string>>
parseURLFlakeRef(const fetchers::settings_t& fetch_settings, const std::string& url,
                 const std::optional<std::filesystem::path>& base_dir, bool is_flake) {
  try {
    auto parsed = parse_url(url, /*lenient=*/true);
    if (base_dir && (parsed.scheme() == "path" || parsed.scheme() == "git+file")) {
      /* Here we know that the path must not contain encoded '/' or NUL bytes. */
      auto path = render_url_path_ensure_legal(parsed.path());
      if (!is_absolute(path))
        parsed.set_path(
            split_string<std::vector<std::string>>(abs_path(path, base_dir->string()), "/"));
    }
    return from_parsed_url(fetch_settings, std::move(parsed), is_flake);
  } catch (BadURL&) {
    return std::nullopt;
  }
}

std::pair<flake_ref_t, std::string>
parse_flake_ref_with_fragment(const fetchers::settings_t& fetch_settings, const std::string& url,
                              const std::optional<std::filesystem::path>& base_dir,
                              bool allow_missing, bool is_flake, bool preserve_relative_paths) {
  using namespace fetchers;

  if (auto res = parseFlakeIdRef(fetch_settings, url, is_flake)) {
    return *res;
  } else if (auto res = parseURLFlakeRef(fetch_settings, url, base_dir, is_flake)) {
    return *res;
  } else {
    return parse_path_flake_ref_with_fragment(fetch_settings, url, base_dir, allow_missing,
                                              is_flake, preserve_relative_paths);
  }
}

flake_ref_t flake_ref_t::fromAttrs(const fetchers::settings_t& fetch_settings,
                                   const fetchers::Attrs& attrs) {
  auto attrs2(attrs);
  attrs2.erase("dir");
  return flake_ref_t(fetchers::input_t::fromAttrs(fetch_settings, std::move(attrs2)),
                     fetchers::maybe_get_str_attr(attrs, "dir").value_or(""));
}

std::pair<ref<source_accessor_t>, flake_ref_t>
flake_ref_t::lazyFetch(const fetchers::settings_t& fetch_settings, store_t& store) const {
  auto [accessor, lockedInput] = input.get_accessor(fetch_settings, store);
  return {accessor, flake_ref_t(std::move(lockedInput), subdir)};
}

flake_ref_t flake_ref_t::canonicalize() const {
  auto flake_ref(*this);

  /* Backward compatibility hack: In old versions of Nix, if you had
     a flake input like

       inputs.foo.url = "git+https://foo/bar?dir=subdir";

     it would result in a lock file entry like

       "original": {
         "dir": "subdir",
         "type": "git",
         "url": "https://foo/bar?dir=subdir"
       }

     New versions of Nix remove `?dir=subdir` from the `url` field,
     since the subdirectory is intended for `flake_ref_t`, not the
     fetcher (and specifically the remote server), that is, the
     flakeref is parsed into

       "original": {
         "dir": "subdir",
         "type": "git",
         "url": "https://foo/bar"
       }

     However, this causes new versions of Nix to consider the lock
     file entry to be stale since the `original` ref no longer
     matches exactly.

     For this reason, we canonicalise the `original` ref by
     filtering the `dir` query parameter from the URL. */
  if (auto url = fetchers::maybe_get_str_attr(flake_ref.input.attrs, "url")) {
    try {
      auto parsed = parse_url(*url, /*lenient=*/true);
      if (auto dir2 = get(parsed.query(), "dir")) {
        if (flake_ref.subdir != "" && flake_ref.subdir == *dir2)
          parsed.query().erase("dir");
      }
      flake_ref.input.attrs.insert_or_assign("url", parsed.to_string());
    } catch (BadURL&) {
    }
  }

  return flake_ref;
}

std::tuple<flake_ref_t, std::string, ExtendedOutputsSpec>
parse_flake_ref_with_fragment_and_extended_outputs_spec(
    const fetchers::settings_t& fetch_settings, const std::string& url,
    const std::optional<std::filesystem::path>& base_dir, bool allow_missing, bool is_flake) {
  auto [prefix, extendedOutputsSpec] = ExtendedOutputsSpec::parse(url);
  auto [flake_ref, fragment] = parse_flake_ref_with_fragment(fetch_settings, std::string{prefix},
                                                             base_dir, allow_missing, is_flake);
  return {std::move(flake_ref), fragment, std::move(extendedOutputsSpec)};
}

std::regex flake_id_regex(flakeIdRegexS, std::regex::ECMAScript);

} // namespace nix
