#include "nix/flake/url-name.h"

#include <optional>
#include <regex>
#include <string>
#include <vector>

#include "nix/util/url.h"

namespace nix {

static const std::string attribute_name_pattern("[a-zA-Z0-9_-]+");
static const std::regex last_attribute_regex("^((?:" + attribute_name_pattern + "\\.)*)(" +
                                             attribute_name_pattern + ")(\\^.*)?$");
static const std::string path_segment_pattern("[a-zA-Z0-9_-]+");
static const std::regex valid_name_regex("^" + path_segment_pattern + "$");
static const std::regex git_provider_regex("github|gitlab|sourcehut");
static const std::regex git_scheme_regex("git($|\\+.*)");

/**
 * Find the last non-empty path segment that matches the valid name pattern.
 * This handles edge cases like trailing slashes, double slashes, and encoded characters.
 */
static std::optional<std::string>
get_last_valid_path_segment(const std::vector<std::string>& path) {
  for (auto it = path.rbegin(); it != path.rend(); ++it) {
    if (!it->empty() && std::regex_match(*it, valid_name_regex)) {
      return *it;
    }
  }
  return {};
}

/**
 * Get the second path segment (for github/gitlab/sourcehut repo names).
 * Skips empty segments to handle double slashes.
 */
static std::optional<std::string>
get_second_valid_path_segment(const std::vector<std::string>& path) {
  int count = 0;
  for (const auto& segment : path) {
    if (!segment.empty() && std::regex_match(segment, valid_name_regex)) {
      count++;
      if (count == 2) {
        return segment;
      }
    }
  }
  return {};
}

std::optional<std::string> get_name_from_url(const parsed_url_t& url) {
  std::smatch match;

  /* If there is a dir= argument, use its value */
  if (url.query().count("dir") > 0) {
    return url.query().at("dir");
  }

  /* If the fragment isn't a "default" and contains two attribute elements, use the last one */
  if (std::regex_match(url.fragment(), match, last_attribute_regex) &&
      match.str(1) != "defaultPackage." && match.str(2) != "default") {
    return match.str(2);
  }

  const auto& path_segments = url.path();

  /* If this is a github/gitlab/sourcehut flake, use the repo name (second path segment) */
  if (std::regex_match(url.scheme(), git_provider_regex)) {
    if (auto name = get_second_valid_path_segment(path_segments)) {
      return name;
    }
  }

  /* If it is a regular git flake, use the directory name (last valid segment) */
  if (std::regex_match(url.scheme(), git_scheme_regex)) {
    if (auto name = get_last_valid_path_segment(path_segments)) {
      return name;
    }
  }

  /* If there is no fragment, take the last valid element of the path.
     This handles edge cases like trailing slashes, double slashes, and paths
     ending with non-name characters. */
  if (auto name = get_last_valid_path_segment(path_segments)) {
    return name;
  }

  /* If even that didn't work, the URL does not contain enough info to determine a useful name */
  return {};
}

} // namespace nix
