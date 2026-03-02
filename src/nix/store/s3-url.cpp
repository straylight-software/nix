#include "nix/store/s3-url.h"

#include <ranges>
#include <string_view>

#include "nix/util/error.h"
#include "nix/util/split.h"
#include "nix/util/strings-inline.h"

namespace nix {

ParsedS3URL ParsedS3URL::parse(const parsed_url_t& parsed) try {
  if (parsed.scheme() != std::string_view{"s3"}) {
    throw BadURL("URI scheme '%s' is not 's3'", parsed.scheme());
  }

  /* Yeah, S3 URLs in Nix have the bucket name as authority. Luckily registered name type
     authority has the same restrictions (mostly) as S3 bucket names.
     TODO: Validate against:
     https://docs.aws.amazon.com/AmazonS3/latest/userguide/bucketnamingrules.html#general-purpose-bucket-names
     */
  if (!parsed.authority() || parsed.authority()->host().empty() ||
      parsed.authority()->host_type() != parsed_url_t::authority_t::host_type_t::name) {
    throw BadURL("URI has a missing or invalid bucket name");
  }

  /* TODO: Validate the key against:
   * https://docs.aws.amazon.com/AmazonS3/latest/userguide/object-keys.html#object-key-guidelines
   */

  auto getOptionalParam = [&](std::string_view key) -> std::optional<std::string> {
    const auto& query = parsed.query();
    auto it = query.find(key);
    if (it == query.end()) {
      return std::nullopt;
    }
    return it->second;
  };

  auto endpoint = getOptionalParam("endpoint");
  if (parsed.path().size() <= 1 || !parsed.path().front().empty()) {
    throw BadURL("URI has a missing or invalid key");
  }

  auto path = std::views::drop(parsed.path(), 1) | std::ranges::to<std::vector<std::string>>();

  return ParsedS3URL{
      .bucket = parsed.authority()->host(),
      .key = std::move(path),
      .profile = getOptionalParam("profile"),
      .region = getOptionalParam("region"),
      .scheme = getOptionalParam("scheme"),
      .versionId = getOptionalParam("versionId"),
      .endpoint = [&]() -> decltype(ParsedS3URL::endpoint) {
        if (!endpoint) {
          return std::monostate();
        }

        /* Try to parse the endpoint as a full-fledged URL with a scheme. */
        try {
          return parse_url(*endpoint);
        } catch (BadURL&) {
        }

        return parsed_url_t::authority_t::parse(*endpoint);
      }(),
  };
} catch (BadURL& e) {
  e.add_trace({}, "while parsing S3 URI: '%s'", parsed.to_string());
  throw;
}

parsed_url_t ParsedS3URL::toHttpsUrl() const {
  auto to_view = [](const auto& x) { return std::string_view{x}; };

  auto region_str = region.transform(to_view).value_or("us-east-1");
  auto scheme_str = scheme.transform(to_view).value_or("https");

  // Build query parameters (e.g., versionId if present)
  string_map_t query_params;
  if (versionId) {
    query_params["versionId"] = *versionId;
  }

  // Helper to build the path
  auto build_path = [&](std::vector<std::string> base_path) {
    base_path.push_back(bucket);
    base_path.insert(base_path.end(), key.begin(), key.end());
    return base_path;
  };

  // Handle endpoint configuration using std::visit
  return std::visit(overloaded{
                        [&](const std::monostate&) {
                          // No custom endpoint, use standard AWS S3 endpoint
                          parsed_url_t result;
                          result.set_scheme(std::string{scheme_str});
                          parsed_url_t::authority_t auth;
                          auth.set_host("s3." + std::string{region_str} + ".amazonaws.com");
                          result.set_authority(std::move(auth));
                          result.set_path(build_path({""}));
                          result.set_query(query_params);
                          return result;
                        },
                        [&](const parsed_url_t::authority_t& auth) {
                          // Endpoint is just an authority (hostname/port)
                          parsed_url_t result;
                          result.set_scheme(std::string{scheme_str});
                          result.set_authority(auth);
                          result.set_path(build_path({""}));
                          result.set_query(query_params);
                          return result;
                        },
                        [&](const parsed_url_t& endpoint_url) {
                          // Endpoint is already a ParsedURL (e.g., http://server:9000)
                          parsed_url_t result;
                          result.set_scheme(endpoint_url.scheme());
                          result.set_authority(endpoint_url.authority());
                          result.set_path(build_path(endpoint_url.path()));
                          result.set_query(query_params);
                          return result;
                        },
                    },
                    endpoint);
}

} // namespace nix
