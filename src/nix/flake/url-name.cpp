#include "nix/flake/url-name.h"

#include <optional>
#include <regex>
#include <string>

#include "nix/util/strings.h"
#include "nix/util/url.h"

namespace nix {

static const std::string attribute_name_pattern("[a-zA-Z0-9_-]+");
static const std::regex last_attribute_regex("^((?:" + attribute_name_pattern + "\\.)*)(" +
                                             attribute_name_pattern + ")(\\^.*)?$");
static const std::string path_segment_pattern("[a-zA-Z0-9_-]+");
static const std::regex last_path_segment_regex(".*/(" + path_segment_pattern + ")");
static const std::regex second_path_segment_regex("(?:" + path_segment_pattern + ")/(" +
                                                  path_segment_pattern + ")(?:/.*)?");
static const std::regex git_provider_regex("github|gitlab|sourcehut");
static const std::regex git_scheme_regex("git($|\\+.*)");

std::optional<std::string> get_name_from_url(const parsed_url_t& url) {
  std::smatch match;

  /* If there is a dir= argument, use its value */
  if (url.query().count("dir") > 0)
    return url.query().at("dir");

  /* If the fragment isn't a "default" and contains two attribute elements, use the last one */
  if (std::regex_match(url.fragment(), match, last_attribute_regex) &&
      match.str(1) != "defaultPackage." && match.str(2) != "default") {
    return match.str(2);
  }

  /* This is not right, because special chars like slashes within the
     path fragments should be percent encoded, but I don't think any
     of the regexes above care. */
  auto path = concat_strings_sep("/", url.path());

  /* If this is a github/gitlab/sourcehut flake, use the repo name */
  if (std::regex_match(url.scheme(), git_provider_regex) &&
      std::regex_match(path, match, second_path_segment_regex))
    return match.str(1);

  /* If it is a regular git flake, use the directory name */
  if (std::regex_match(url.scheme(), git_scheme_regex) &&
      std::regex_match(path, match, last_path_segment_regex))
    return match.str(1);

  /* If there is no fragment, take the last element of the path */
  if (std::regex_match(path, match, last_path_segment_regex))
    return match.str(1);

  /* If even that didn't work, the URL does not contain enough info to determine a useful name */
  return {};
}

} // namespace nix
