#pragma once
///@file
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "nix/store/config.h"
#include "nix/util/url.h"
#include "nix/util/util.h"

namespace nix {

/**
 * Parsed S3 URL.
 */
struct ParsedS3URL {
  std::string bucket;
  /**
   * @see parsed_url_t::path. This is a vector for the same reason.
   * Unlike parsed_url_t::path this doesn't include the leading empty segment,
   * since the bucket name is necessary.
   */
  std::vector<std::string> key;
  std::optional<std::string> profile;
  std::optional<std::string> region;
  std::optional<std::string> scheme;
  std::optional<std::string> versionId;
  /**
   * The endpoint can be either missing, be an absolute URI (with a scheme like `http:`)
   * or an authority (so an IP address or a registered name).
   */
  std::variant<std::monostate, parsed_url_t, parsed_url_t::authority_t> endpoint;

  std::optional<std::string> getEncodedEndpoint() const {
    return std::visit(overloaded{
                          [](std::monostate) -> std::optional<std::string> { return std::nullopt; },
                          [](const auto& authorityOrUrl) -> std::optional<std::string> {
                            return authorityOrUrl.to_string();
                          },
                      },
                      endpoint);
  }

  static ParsedS3URL parse(const parsed_url_t& uri);

  /**
   * Convert this ParsedS3URL to HTTPS parsed_url_t for use with curl's AWS SigV4 authentication
   */
  parsed_url_t toHttpsUrl() const;

  auto operator<=>(const ParsedS3URL& other) const = default;
};

} // namespace nix
