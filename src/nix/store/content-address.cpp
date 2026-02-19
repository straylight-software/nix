#include "nix/store/content-address.h"

#include "nix/util/args.h"
#include "nix/util/json-utils.h"
#include "nix/util/split.h"

namespace nix {

std::string_view makeFileIngestionPrefix(file_ingestion_method_t m) {
  switch (m) {
    case file_ingestion_method_t::Flat:
      // Not prefixed for back compat
      return "";
    case file_ingestion_method_t::NixArchive:
      return "r:";
    case file_ingestion_method_t::Git:
      experimentalFeatureSettings.require(xp_t::GitHashing);
      return "git:";
    default:
      assert(false);
  }
}

std::string_view ContentAddressMethod::render() const {
  switch (raw) {
    case ContentAddressMethod::raw_t::Text:
      return "text";
    case ContentAddressMethod::raw_t::Flat:
    case ContentAddressMethod::raw_t::NixArchive:
    case ContentAddressMethod::raw_t::Git:
      return renderFileIngestionMethod(getFileIngestionMethod());
    default:
      assert(false);
  }
}

/**
 * **Not surjective**
 *
 * This is not exposed because `file_ingestion_method_t::Flat` maps to
 * `ContentAddressMethod::raw_t::Flat` and
 * `ContentAddressMethod::raw_t::Text` alike. We can thus only safely use
 * this when the latter is ruled out (e.g. because it is already
 * handled).
 */
static ContentAddressMethod fileIngestionMethodToContentAddressMethod(file_ingestion_method_t m) {
  switch (m) {
    case file_ingestion_method_t::Flat:
      return ContentAddressMethod::raw_t::Flat;
    case file_ingestion_method_t::NixArchive:
      return ContentAddressMethod::raw_t::NixArchive;
    case file_ingestion_method_t::Git:
      return ContentAddressMethod::raw_t::Git;
    default:
      assert(false);
  }
}

ContentAddressMethod ContentAddressMethod::parse(std::string_view m) {
  if (m == "text")
    return ContentAddressMethod::raw_t::Text;
  else
    return fileIngestionMethodToContentAddressMethod(parseFileIngestionMethod(m));
}

std::string_view ContentAddressMethod::renderPrefix() const {
  switch (raw) {
    case ContentAddressMethod::raw_t::Text:
      return "text:";
    case ContentAddressMethod::raw_t::Flat:
    case ContentAddressMethod::raw_t::NixArchive:
    case ContentAddressMethod::raw_t::Git:
      return makeFileIngestionPrefix(getFileIngestionMethod());
    default:
      assert(false);
  }
}

ContentAddressMethod ContentAddressMethod::parsePrefix(std::string_view& m) {
  if (splitPrefix(m, "r:")) {
    return ContentAddressMethod::raw_t::NixArchive;
  } else if (splitPrefix(m, "git:")) {
    experimentalFeatureSettings.require(xp_t::GitHashing);
    return ContentAddressMethod::raw_t::Git;
  } else if (splitPrefix(m, "text:")) {
    return ContentAddressMethod::raw_t::Text;
  }
  return ContentAddressMethod::raw_t::Flat;
}

/**
 * This is slightly more mindful of forward compat in that it uses `fixed:`
 * rather than just doing a raw empty prefix or `r:`, which doesn't "save room"
 * for future changes very well.
 */
static std::string renderPrefixModern(const ContentAddressMethod& ca) {
  switch (ca.raw) {
    case ContentAddressMethod::raw_t::Text:
      return "text:";
    case ContentAddressMethod::raw_t::Flat:
    case ContentAddressMethod::raw_t::NixArchive:
    case ContentAddressMethod::raw_t::Git:
      return "fixed:" + makeFileIngestionPrefix(ca.getFileIngestionMethod());
    default:
      assert(false);
  }
}

std::string ContentAddressMethod::renderWithAlgo(hash_algorithm_t ha) const {
  return renderPrefixModern(*this) + printHashAlgo(ha);
}

file_ingestion_method_t ContentAddressMethod::getFileIngestionMethod() const {
  switch (raw) {
    case ContentAddressMethod::raw_t::Flat:
      return file_ingestion_method_t::Flat;
    case ContentAddressMethod::raw_t::NixArchive:
      return file_ingestion_method_t::NixArchive;
    case ContentAddressMethod::raw_t::Git:
      return file_ingestion_method_t::Git;
    case ContentAddressMethod::raw_t::Text:
      return file_ingestion_method_t::Flat;
    default:
      assert(false);
  }
}

std::string ContentAddress::render() const {
  return renderPrefixModern(method) + this->hash.to_string(hash_format_t::Nix32, true);
}

/**
 * Parses content address strings up to the hash.
 */
static std::pair<ContentAddressMethod, hash_algorithm_t>
parseContentAddressMethodPrefix(std::string_view& rest) {
  std::string_view wholeInput{rest};

  std::string_view prefix;
  {
    auto optPrefix = splitPrefixTo(rest, ':');
    if (!optPrefix)
      throw UsageError("not a content address because it is not in the form '<prefix>:<rest>': %s",
                       wholeInput);
    prefix = *optPrefix;
  }

  auto parseHashAlgorithm_ = [&]() {
    auto hashAlgoRaw = splitPrefixTo(rest, ':');
    if (!hashAlgoRaw)
      throw UsageError("content address hash must be in form '<algo>:<hash>', but found: %s",
                       wholeInput);
    hash_algorithm_t hashAlgo = parseHashAlgo(*hashAlgoRaw);
    return hashAlgo;
  };

  // Switch on prefix
  if (prefix == "text") {
    // No parsing of the ingestion method, "text" only support flat.
    hash_algorithm_t hashAlgo = parseHashAlgorithm_();
    return {
        ContentAddressMethod::raw_t::Text,
        std::move(hashAlgo),
    };
  } else if (prefix == "fixed") {
    // Parse method
    auto method = ContentAddressMethod::raw_t::Flat;
    if (splitPrefix(rest, "r:"))
      method = ContentAddressMethod::raw_t::NixArchive;
    else if (splitPrefix(rest, "git:")) {
      experimentalFeatureSettings.require(xp_t::GitHashing);
      method = ContentAddressMethod::raw_t::Git;
    }
    hash_algorithm_t hashAlgo = parseHashAlgorithm_();
    return {
        std::move(method),
        std::move(hashAlgo),
    };
  } else
    throw UsageError(
        "content address prefix '%s' is unrecognized. Recogonized prefixes are 'text' or 'fixed'",
        prefix);
}

ContentAddress ContentAddress::parse(std::string_view rawCa) {
  auto rest = rawCa;

  auto [caMethod, hashAlgo] = parseContentAddressMethodPrefix(rest);

  return ContentAddress{
      .method = std::move(caMethod),
      .hash = Hash::parseNonSRIUnprefixed(rest, hashAlgo),
  };
}

std::pair<ContentAddressMethod, hash_algorithm_t>
ContentAddressMethod::parseWithAlgo(std::string_view caMethod) {
  std::string asPrefix = std::string{caMethod} + ":";
  // parseContentAddressMethodPrefix takes its argument by reference
  std::string_view asPrefixView = asPrefix;
  return parseContentAddressMethodPrefix(asPrefixView);
}

std::optional<ContentAddress> ContentAddress::parseOpt(std::string_view rawCaOpt) {
  return rawCaOpt == "" ? std::nullopt : std::optional{ContentAddress::parse(rawCaOpt)};
};

std::string renderContentAddress(std::optional<ContentAddress> ca) {
  return ca ? ca->render() : "";
}

std::string ContentAddress::printMethodAlgo() const {
  return std::string{method.renderPrefix()} + printHashAlgo(hash.algo);
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
    case ContentAddressMethod::raw_t::Flat:
    case ContentAddressMethod::raw_t::NixArchive:
    case ContentAddressMethod::raw_t::Git:
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
    case ContentAddressMethod::raw_t::Flat:
    case ContentAddressMethod::raw_t::NixArchive:
    case ContentAddressMethod::raw_t::Git:
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
                          return fileIngestionMethodToContentAddressMethod(fsh.method);
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
  return ContentAddressMethod::parse(getString(json));
}

void adl_serializer<ContentAddressMethod>::to_json(json& json, const ContentAddressMethod& m) {
  json = m.render();
}

ContentAddress adl_serializer<ContentAddress>::from_json(const json& json) {
  auto obj = getObject(json);
  return {
      .method = adl_serializer<ContentAddressMethod>::from_json(valueAt(obj, "method")),
      .hash = valueAt(obj, "hash"),
  };
}

void adl_serializer<ContentAddress>::to_json(json& json, const ContentAddress& ca) {
  json = {
      {"method", ca.method},
      {"hash", ca.hash},
  };
}

} // namespace nlohmann
