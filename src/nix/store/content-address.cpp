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

std::string_view ContentAddressMethod::render() const {
  switch (raw) {
    case ContentAddressMethod::raw_t::Text:
      return "text";
    case ContentAddressMethod::raw_t::flat:
    case ContentAddressMethod::raw_t::nix_archive:
    case ContentAddressMethod::raw_t::git:
      return render_file_ingestion_method(getFileIngestionMethod());
    default:
      assert(false);
  }
}

/**
 * **Not surjective**
 *
 * This is not exposed because `file_ingestion_method_t::flat` maps to
 * `ContentAddressMethod::raw_t::flat` and
 * `ContentAddressMethod::raw_t::Text` alike. We can thus only safely use
 * this when the latter is ruled out (e.g. because it is already
 * handled).
 */
static ContentAddressMethod file_ingestion_method_to_content_address_method(file_ingestion_method_t m) {
  switch (m) {
    case file_ingestion_method_t::flat:
      return ContentAddressMethod::raw_t::flat;
    case file_ingestion_method_t::nix_archive:
      return ContentAddressMethod::raw_t::nix_archive;
    case file_ingestion_method_t::git:
      return ContentAddressMethod::raw_t::git;
    default:
      assert(false);
  }
}

ContentAddressMethod ContentAddressMethod::parse(std::string_view m) {
  if (m == "text")
    return ContentAddressMethod::raw_t::Text;
  else
    return file_ingestion_method_to_content_address_method(parse_file_ingestion_method(m));
}

std::string_view ContentAddressMethod::renderPrefix() const {
  switch (raw) {
    case ContentAddressMethod::raw_t::Text:
      return "text:";
    case ContentAddressMethod::raw_t::flat:
    case ContentAddressMethod::raw_t::nix_archive:
    case ContentAddressMethod::raw_t::git:
      return make_file_ingestion_prefix(getFileIngestionMethod());
    default:
      assert(false);
  }
}

ContentAddressMethod ContentAddressMethod::parsePrefix(std::string_view& m) {
  if (split_prefix(m, "r:")) {
    return ContentAddressMethod::raw_t::nix_archive;
  } else if (split_prefix(m, "git:")) {
    experimental_feature_settings.require(xp_t::git_hashing);
    return ContentAddressMethod::raw_t::git;
  } else if (split_prefix(m, "text:")) {
    return ContentAddressMethod::raw_t::Text;
  }
  return ContentAddressMethod::raw_t::flat;
}

/**
 * This is slightly more mindful of forward compat in that it uses `fixed:`
 * rather than just doing a raw empty prefix or `r:`, which doesn't "save room"
 * for future changes very well.
 */
static std::string render_prefix_modern(const ContentAddressMethod& ca) {
  switch (ca.raw) {
    case ContentAddressMethod::raw_t::Text:
      return "text:";
    case ContentAddressMethod::raw_t::flat:
    case ContentAddressMethod::raw_t::nix_archive:
    case ContentAddressMethod::raw_t::git:
      return "fixed:" + make_file_ingestion_prefix(ca.getFileIngestionMethod());
    default:
      assert(false);
  }
}

std::string ContentAddressMethod::renderWithAlgo(hash_algorithm_t ha) const {
  return render_prefix_modern(*this) + print_hash_algo(ha);
}

file_ingestion_method_t ContentAddressMethod::getFileIngestionMethod() const {
  switch (raw) {
    case ContentAddressMethod::raw_t::flat:
      return file_ingestion_method_t::flat;
    case ContentAddressMethod::raw_t::nix_archive:
      return file_ingestion_method_t::nix_archive;
    case ContentAddressMethod::raw_t::git:
      return file_ingestion_method_t::git;
    case ContentAddressMethod::raw_t::Text:
      return file_ingestion_method_t::flat;
    default:
      assert(false);
  }
}

std::string ContentAddress::render() const {
  return render_prefix_modern(method) + this->hash.to_string(hash_format_t::nix32, true);
}

/**
 * Parses content address strings up to the hash.
 */
static std::pair<ContentAddressMethod, hash_algorithm_t>
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
        ContentAddressMethod::raw_t::Text,
        std::move(hash_algo),
    };
  } else if (prefix == "fixed") {
    // Parse method
    auto method = ContentAddressMethod::raw_t::flat;
    if (split_prefix(rest, "r:"))
      method = ContentAddressMethod::raw_t::nix_archive;
    else if (split_prefix(rest, "git:")) {
      experimental_feature_settings.require(xp_t::git_hashing);
      method = ContentAddressMethod::raw_t::git;
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

ContentAddress ContentAddress::parse(std::string_view rawCa) {
  auto rest = rawCa;

  auto [ca_method, hash_algo] = parse_content_address_method_prefix(rest);

  return ContentAddress{
      .method = std::move(ca_method),
      .hash = Hash::parse_non_sri_unprefixed(rest, hash_algo),
  };
}

std::pair<ContentAddressMethod, hash_algorithm_t>
ContentAddressMethod::parseWithAlgo(std::string_view ca_method) {
  std::string asPrefix = std::string{ca_method} + ":";
  // parseContentAddressMethodPrefix takes its argument by reference
  std::string_view asPrefixView = asPrefix;
  return parse_content_address_method_prefix(asPrefixView);
}

std::optional<ContentAddress> ContentAddress::parseOpt(std::string_view rawCaOpt) {
  return rawCaOpt == "" ? std::nullopt : std::optional{ContentAddress::parse(rawCaOpt)};
};

std::string render_content_address(std::optional<ContentAddress> ca) {
  return ca ? ca->render() : "";
}

std::string ContentAddress::printMethodAlgo() const {
  return std::string{method.renderPrefix()} + print_hash_algo(hash.algo);
}

bool StoreReferences::empty() const {
  return !self && others.empty();
}

size_t StoreReferences::size() const {
  return (self ? 1 : 0) + others.size();
}

ContentAddressWithReferences
ContentAddressWithReferences::withoutRefs(const ContentAddress& ca) noexcept {
  switch (ca.method.raw) {
    case ContentAddressMethod::raw_t::Text:
      return TextInfo{
          .hash = ca.hash,
          .references = {},
      };
    case ContentAddressMethod::raw_t::flat:
    case ContentAddressMethod::raw_t::nix_archive:
    case ContentAddressMethod::raw_t::git:
      return FixedOutputInfo{
          .method = ca.method.getFileIngestionMethod(),
          .hash = ca.hash,
          .references = {},
      };
    default:
      assert(false);
  }
}

ContentAddressWithReferences ContentAddressWithReferences::fromParts(ContentAddressMethod method,
                                                                     Hash hash,
                                                                     StoreReferences refs) {
  switch (method.raw) {
    case ContentAddressMethod::raw_t::Text:
      if (refs.self)
        throw Error("self-reference not allowed with text hashing");
      return TextInfo{
          .hash = std::move(hash),
          .references = std::move(refs.others),
      };
    case ContentAddressMethod::raw_t::flat:
    case ContentAddressMethod::raw_t::nix_archive:
    case ContentAddressMethod::raw_t::git:
      return FixedOutputInfo{
          .method = method.getFileIngestionMethod(),
          .hash = std::move(hash),
          .references = std::move(refs),
      };
    default:
      assert(false);
  }
}

ContentAddressMethod ContentAddressWithReferences::getMethod() const {
  return std::visit(overloaded{
                        [](const TextInfo& th) -> ContentAddressMethod {
                          return ContentAddressMethod::raw_t::Text;
                        },
                        [](const FixedOutputInfo& fsh) -> ContentAddressMethod {
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

ContentAddressMethod adl_serializer<ContentAddressMethod>::from_json(const json& json) {
  return ContentAddressMethod::parse(get_string(json));
}

void adl_serializer<ContentAddressMethod>::to_json(json& json, const ContentAddressMethod& m) {
  json = m.render();
}

ContentAddress adl_serializer<ContentAddress>::from_json(const json& json) {
  auto obj = get_object(json);
  return {
      .method = adl_serializer<ContentAddressMethod>::from_json(value_at(obj, "method")),
      .hash = value_at(obj, "hash"),
  };
}

void adl_serializer<ContentAddress>::to_json(json& json, const ContentAddress& ca) {
  json = {
      {"method", ca.method},
      {"hash", ca.hash},
  };
}

} // namespace nlohmann
