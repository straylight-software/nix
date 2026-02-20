#include "nix/store/content-address.h"

#include "nix/util/args.h"
#include "nix/util/json-utils.h"
#include "nix/util/split.h"

namespace nix {

std::string_view make_file_ingestion_prefix(file_ingestion_method_t m) {
  switch (m) {
    case file_ingestion_method_t::flat:
      // Not prefixed for back compat
      return "";
    case file_ingestion_method_t::nix_archive:
      return "r:";
    case file_ingestion_method_t::git:
      experimental_feature_settings.require(xp_t::git_hashing);
      return "git:";
    default:
      assert(false);
  }
}

std::string_view content_address_method_t::render() const {
  switch (raw) {
    case content_address_method_t::raw_t::Text:
      return "text";
    case content_address_method_t::raw_t::flat:
    case content_address_method_t::raw_t::nix_archive:
    case content_address_method_t::raw_t::git:
      return render_file_ingestion_method(getFileIngestionMethod());
    default:
      assert(false);
  }
}

/**
 * **Not surjective**
 *
 * This is not exposed because `file_ingestion_method_t::flat` maps to
 * `content_address_method_t::raw_t::flat` and
 * `content_address_method_t::raw_t::Text` alike. We can thus only safely use
 * this when the latter is ruled out (e.g. because it is already
 * handled).
 */
static content_address_method_t file_ingestion_method_to_content_address_method(file_ingestion_method_t m) {
  switch (m) {
    case file_ingestion_method_t::flat:
      return content_address_method_t::raw_t::flat;
    case file_ingestion_method_t::nix_archive:
      return content_address_method_t::raw_t::nix_archive;
    case file_ingestion_method_t::git:
      return content_address_method_t::raw_t::git;
    default:
      assert(false);
  }
}

content_address_method_t content_address_method_t::parse(std::string_view m) {
  if (m == "text")
    return content_address_method_t::raw_t::Text;
  else
    return file_ingestion_method_to_content_address_method(parse_file_ingestion_method(m));
}

std::string_view content_address_method_t::renderPrefix() const {
  switch (raw) {
    case content_address_method_t::raw_t::Text:
      return "text:";
    case content_address_method_t::raw_t::flat:
    case content_address_method_t::raw_t::nix_archive:
    case content_address_method_t::raw_t::git:
      return make_file_ingestion_prefix(getFileIngestionMethod());
    default:
      assert(false);
  }
}

content_address_method_t content_address_method_t::parsePrefix(std::string_view& m) {
  if (split_prefix(m, "r:")) {
    return content_address_method_t::raw_t::nix_archive;
  } else if (split_prefix(m, "git:")) {
    experimental_feature_settings.require(xp_t::git_hashing);
    return content_address_method_t::raw_t::git;
  } else if (split_prefix(m, "text:")) {
    return content_address_method_t::raw_t::Text;
  }
  return content_address_method_t::raw_t::flat;
}

/**
 * This is slightly more mindful of forward compat in that it uses `fixed:`
 * rather than just doing a raw empty prefix or `r:`, which doesn't "save room"
 * for future changes very well.
 */
static std::string render_prefix_modern(const content_address_method_t& ca) {
  switch (ca.raw) {
    case content_address_method_t::raw_t::Text:
      return "text:";
    case content_address_method_t::raw_t::flat:
    case content_address_method_t::raw_t::nix_archive:
    case content_address_method_t::raw_t::git:
      return "fixed:" + make_file_ingestion_prefix(ca.getFileIngestionMethod());
    default:
      assert(false);
  }
}

std::string content_address_method_t::renderWithAlgo(hash_algorithm_t ha) const {
  return render_prefix_modern(*this) + print_hash_algo(ha);
}

file_ingestion_method_t content_address_method_t::getFileIngestionMethod() const {
  switch (raw) {
    case content_address_method_t::raw_t::flat:
      return file_ingestion_method_t::flat;
    case content_address_method_t::raw_t::nix_archive:
      return file_ingestion_method_t::nix_archive;
    case content_address_method_t::raw_t::git:
      return file_ingestion_method_t::git;
    case content_address_method_t::raw_t::Text:
      return file_ingestion_method_t::flat;
    default:
      assert(false);
  }
}

std::string content_address_t::render() const {
  return render_prefix_modern(method) + this->hash.to_string(hash_format_t::nix32, true);
}

/**
 * Parses content address strings up to the hash.
 */
static std::pair<content_address_method_t, hash_algorithm_t>
parse_content_address_method_prefix(std::string_view& rest) {
  std::string_view whole_input{rest};

  std::string_view prefix;
  {
    auto opt_prefix = split_prefix_to(rest, ':');
    if (!opt_prefix)
      throw UsageError("not a content address because it is not in the form '<prefix>:<rest>': %s",
                       whole_input);
    prefix = *opt_prefix;
  }

  auto parse_hash_algorithm_ = [&]() {
    auto hash_algo_raw = split_prefix_to(rest, ':');
    if (!hash_algo_raw)
      throw UsageError("content address hash must be in form '<algo>:<hash>', but found: %s",
                       whole_input);
    hash_algorithm_t hash_algo = parse_hash_algo(*hash_algo_raw);
    return hash_algo;
  };

  // Switch on prefix
  if (prefix == "text") {
    // No parsing of the ingestion method, "text" only support flat.
    hash_algorithm_t hash_algo = parse_hash_algorithm_();
    return {
        content_address_method_t::raw_t::Text,
        std::move(hash_algo),
    };
  } else if (prefix == "fixed") {
    // Parse method
    auto method = content_address_method_t::raw_t::flat;
    if (split_prefix(rest, "r:"))
      method = content_address_method_t::raw_t::nix_archive;
    else if (split_prefix(rest, "git:")) {
      experimental_feature_settings.require(xp_t::git_hashing);
      method = content_address_method_t::raw_t::git;
    }
    hash_algorithm_t hash_algo = parse_hash_algorithm_();
    return {
        std::move(method),
        std::move(hash_algo),
    };
  } else
    throw UsageError(
        "content address prefix '%s' is unrecognized. Recogonized prefixes are 'text' or 'fixed'",
        prefix);
}

content_address_t content_address_t::parse(std::string_view rawCa) {
  auto rest = rawCa;

  auto [ca_method, hash_algo] = parse_content_address_method_prefix(rest);

  return content_address_t{
      .method = std::move(ca_method),
      .hash = Hash::parse_non_sri_unprefixed(rest, hash_algo),
  };
}

std::pair<content_address_method_t, hash_algorithm_t>
content_address_method_t::parseWithAlgo(std::string_view ca_method) {
  std::string asPrefix = std::string{ca_method} + ":";
  // parseContentAddressMethodPrefix takes its argument by reference
  std::string_view asPrefixView = asPrefix;
  return parse_content_address_method_prefix(asPrefixView);
}

std::optional<content_address_t> content_address_t::parseOpt(std::string_view rawCaOpt) {
  return rawCaOpt == "" ? std::nullopt : std::optional{content_address_t::parse(rawCaOpt)};
};

std::string render_content_address(std::optional<content_address_t> ca) {
  return ca ? ca->render() : "";
}

std::string content_address_t::printMethodAlgo() const {
  return std::string{method.renderPrefix()} + print_hash_algo(hash.algo());
}

bool store_references_t::empty() const {
  return !self && others.empty();
}

size_t store_references_t::size() const {
  return (self ? 1 : 0) + others.size();
}

ContentAddressWithReferences
ContentAddressWithReferences::withoutRefs(const content_address_t& ca) noexcept {
  switch (ca.method.raw) {
    case content_address_method_t::raw_t::Text:
      return TextInfo{
          .hash = ca.hash,
          .references = {},
      };
    case content_address_method_t::raw_t::flat:
    case content_address_method_t::raw_t::nix_archive:
    case content_address_method_t::raw_t::git:
      return FixedOutputInfo{
          .method = ca.method.getFileIngestionMethod(),
          .hash = ca.hash,
          .references = {},
      };
    default:
      assert(false);
  }
}

ContentAddressWithReferences ContentAddressWithReferences::fromParts(content_address_method_t method,
                                                                     Hash hash,
                                                                     store_references_t refs) {
  switch (method.raw) {
    case content_address_method_t::raw_t::Text:
      if (refs.self)
        throw Error("self-reference not allowed with text hashing");
      return TextInfo{
          .hash = std::move(hash),
          .references = std::move(refs.others),
      };
    case content_address_method_t::raw_t::flat:
    case content_address_method_t::raw_t::nix_archive:
    case content_address_method_t::raw_t::git:
      return FixedOutputInfo{
          .method = method.getFileIngestionMethod(),
          .hash = std::move(hash),
          .references = std::move(refs),
      };
    default:
      assert(false);
  }
}

content_address_method_t ContentAddressWithReferences::getMethod() const {
  return std::visit(overloaded{
                        [](const TextInfo& th) -> content_address_method_t {
                          return content_address_method_t::raw_t::Text;
                        },
                        [](const FixedOutputInfo& fsh) -> content_address_method_t {
                          return file_ingestion_method_to_content_address_method(fsh.method);
                        },
                    },
                    raw);
}

Hash ContentAddressWithReferences::getHash() const {
  return std::visit(overloaded{
                        [](const TextInfo& th) { return th.hash; },
                        [](const FixedOutputInfo& fsh) { return fsh.hash; },
                    },
                    raw);
}

} // namespace nix

namespace nlohmann {

using namespace nix;

content_address_method_t adl_serializer<content_address_method_t>::from_json(const json& json) {
  return content_address_method_t::parse(get_string(json));
}

void adl_serializer<content_address_method_t>::to_json(json& json, const content_address_method_t& m) {
  json = m.render();
}

content_address_t adl_serializer<content_address_t>::from_json(const json& json) {
  auto obj = get_object(json);
  return {
      .method = adl_serializer<content_address_method_t>::from_json(value_at(obj, "method")),
      .hash = value_at(obj, "hash"),
  };
}

void adl_serializer<content_address_t>::to_json(json& json, const content_address_t& ca) {
  json = {
      {"method", ca.method},
      {"hash", ca.hash},
  };
}

} // namespace nlohmann
