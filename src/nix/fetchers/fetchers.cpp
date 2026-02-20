#include "nix/fetchers/fetchers.h"

#include <nlohmann/json.hpp>

#include "nix/fetchers/fetch-settings.h"
#include "nix/fetchers/fetch-to-store.h"
#include "nix/store/store-api.h"
#include "nix/util/archive.h"
#include "nix/util/forwarding-source-accessor.h"
#include "nix/util/json-utils.h"
#include "nix/util/source-path.h"
#include "nix/util/url.h"

namespace nix::fetchers {

using InputSchemeMap = std::map<std::string_view, std::shared_ptr<input_scheme_t>>;

static InputSchemeMap& input_schemes() {
  static InputSchemeMap input_scheme_map;
  return input_scheme_map;
}

void register_input_scheme(std::shared_ptr<input_scheme_t>&& inputScheme) {
  auto schemeName = inputScheme->schemeName();
  if (!input_schemes().emplace(schemeName, std::move(inputScheme)).second)
    throw Error("input_t scheme with name %s already registered", schemeName);
}

const InputSchemeMap& get_all_input_schemes() {
  return input_schemes();
}

input_t input_t::fromURL(const settings_t& settings, const std::string& url, bool require_tree) {
  return fromURL(settings, parse_url(url), require_tree);
}

static void fixup_input(input_t& input) {
  // Check common attributes.
  input.getType();
  input.getRef();
  input.get_rev_count();
  input.get_last_modified();
}

input_t input_t::fromURL(const settings_t& settings, const parsed_url_t& url, bool require_tree) {
  for (auto& [_, inputScheme] : input_schemes()) {
    auto res = inputScheme->inputFromURL(settings, url, require_tree);
    if (res) {
      experimental_feature_settings.require(inputScheme->experimental_feature());
      res->scheme = inputScheme;
      fixup_input(*res);
      return std::move(*res);
    }
  }

  // Provide a helpful hint when user tries file+git instead of git+file
  auto parsedScheme = parse_url_scheme(url.scheme());
  if (parsedScheme.application() == "file" && parsedScheme.transport() == "git") {
    throw Error("input '%s' is unsupported; did you mean 'git+file' instead of 'file+git'?", url);
  }

  throw Error("input '%s' is unsupported", url);
}

input_t input_t::fromAttrs(const settings_t& settings, Attrs&& attrs) {
  auto schemeName = ({
    auto schemeNameOpt = maybe_get_str_attr(attrs, "type");
    if (!schemeNameOpt)
      throw Error("'type' attribute to specify input scheme is required but not provided");
    *std::move(schemeNameOpt);
  });

  auto raw = [&]() {
    // Return an input without a scheme; most operations will fail,
    // but not all of them. Doing this is to support those other
    // operations which are supposed to be robust on
    // unknown/uninterpretable inputs.
    input_t input;
    input.attrs = attrs;
    fixup_input(input);
    return input;
  };

  std::shared_ptr<input_scheme_t> inputScheme = ({
    auto i = get(input_schemes(), schemeName);
    i ? *i : nullptr;
  });

  if (!inputScheme)
    return raw();

  experimental_feature_settings.require(inputScheme->experimental_feature());

  auto allowed_attrs = inputScheme->allowed_attrs();

  for (auto& [name, _] : attrs)
    if (name != "type" && name != "__final" && allowed_attrs.count(name) == 0)
      throw Error("input attribute '%s' not supported by scheme '%s'", name, schemeName);

  auto res = inputScheme->inputFromAttrs(settings, attrs);
  if (!res)
    return raw();
  res->scheme = inputScheme;
  fixup_input(*res);
  return std::move(*res);
}

std::optional<std::string> input_t::get_fingerprint(store_t& store) const {
  if (!scheme)
    return std::nullopt;

  if (cachedFingerprint)
    return *cachedFingerprint;

  auto fingerprint = scheme->get_fingerprint(store, *this);

  cachedFingerprint = fingerprint;

  return fingerprint;
}

parsed_url_t input_t::toURL(bool abbreviate) const {
  if (!scheme)
    throw Error("cannot show unsupported input '%s'", attrs_to_json(attrs));

  auto url = scheme->toURL(*this, abbreviate);

  if (abbreviate)
    url.query().erase("narHash");

  return url;
}

std::string input_t::toURLString(const string_map_t& extraQuery, bool abbreviate) const {
  auto url = toURL(abbreviate);
  for (auto& attr : extraQuery)
    url.query().insert(attr);
  return url.to_string();
}

std::string input_t::to_string(bool abbreviate) const {
  return toURL(abbreviate).to_string();
}

bool input_t::isDirect() const {
  return !scheme || scheme->isDirect(*this);
}

bool input_t::isLocked(const settings_t& settings) const {
  return scheme && scheme->isLocked(settings, *this);
}

bool input_t::isFinal() const {
  return maybe_get_bool_attr(attrs, "__final").value_or(false);
}

std::optional<std::string> input_t::isRelative() const {
  assert(scheme);
  return scheme->isRelative(*this);
}

Attrs input_t::toAttrs() const {
  return attrs;
}

bool input_t::operator==(const input_t& other) const noexcept {
  return attrs == other.attrs;
}

bool input_t::contains(const input_t& other) const {
  if (*this == other)
    return true;
  auto other2(other);
  other2.attrs.erase("ref");
  other2.attrs.erase("rev");
  if (*this == other2)
    return true;
  return false;
}

// FIXME: remove
std::tuple<store_path_t, ref<source_accessor_t>, input_t>
input_t::fetch_to_store(const settings_t& settings, store_t& store) const {
  if (!scheme)
    throw Error("cannot fetch unsupported input '%s'", attrs_to_json(toAttrs()));

  try {
    auto [accessor, result] = getAccessorUnchecked(settings, store);

    auto store_path = nix::fetch_to_store(settings, store, source_path_t(accessor), FetchMode::Copy,
                                          result.get_name());

    auto nar_hash = store.queryPathInfo(store_path)->nar_hash;
    result.attrs.insert_or_assign("narHash", nar_hash.to_string(hash_format_t::sri, true));

    result.attrs.insert_or_assign("__final", explicit_t<bool>(true));

    assert(result.isFinal());

    checkLocks(*this, result);

    return {std::move(store_path), accessor, result};
  } catch (Error& e) {
    e.add_trace({}, "while fetching the input '%s'", to_string());
    throw;
  }
}

void input_t::checkLocks(input_t specified, input_t& result) {
  /* If the original input is final, then we just return the
     original attributes, dropping any new fields returned by the
     fetcher. However, any fields that are in both the specified and
     result input must be identical. */
  if (specified.isFinal()) {
    /* Backwards compatibility hack: we had some lock files in the
       past that 'narHash' fields with incorrect base-64
       formatting (lacking the trailing '=', e.g. 'sha256-ri...Mw'
       instead of ''sha256-ri...Mw='). So fix that. */
    if (auto prevNarHash = specified.getNarHash())
      specified.attrs.insert_or_assign("narHash", prevNarHash->to_string(hash_format_t::sri, true));

    if (auto nar_hash = result.getNarHash())
      result.attrs.insert_or_assign("narHash", nar_hash->to_string(hash_format_t::sri, true));

    for (auto& field : specified.attrs) {
      auto field2 = result.attrs.find(field.first);
      if (field2 != result.attrs.end() && field.second != field2->second)
        throw Error("mismatch in field '%s' of input '%s', got '%s'", field.first,
                    attrs_to_json(specified.attrs), attrs_to_json(result.attrs));
    }

    result.attrs = specified.attrs;

    return;
  }

  if (auto prevNarHash = specified.getNarHash()) {
    if (result.getNarHash() != prevNarHash) {
      if (result.getNarHash())
        throw Error((unsigned int)102,
                    "NAR hash mismatch in input '%s', expected '%s' but got '%s'",
                    specified.to_string(), prevNarHash->to_string(hash_format_t::sri, true),
                    result.getNarHash()->to_string(hash_format_t::sri, true));
      else
        throw Error((unsigned int)102,
                    "NAR hash mismatch in input '%s', expected '%s' but got none",
                    specified.to_string(), prevNarHash->to_string(hash_format_t::sri, true));
    }
  }

  if (auto prevRev = specified.getRev()) {
    if (result.getRev() != prevRev)
      throw Error("'rev' attribute mismatch in input '%s', expected %s", result.to_string(),
                  prevRev->git_rev());
  }
}

std::pair<ref<source_accessor_t>, input_t> input_t::get_accessor(const settings_t& settings,
                                                                 store_t& store) const {
  try {
    auto [accessor, result] = getAccessorUnchecked(settings, store);

    result.attrs.insert_or_assign("__final", explicit_t<bool>(true));

    checkLocks(*this, result);

    return {accessor, std::move(result)};
  } catch (Error& e) {
    e.add_trace({}, "while fetching the input '%s'", to_string());
    throw;
  }
}

/**
 * Helper class that ensures that paths in substituted source trees
 * are rendered as `«input»/path` rather than
 * `«input»/nix/store/<hash>-source/path`.
 */
struct substituted_source_accessor_t : forwarding_source_accessor_t {
  using forwarding_source_accessor_t::forwarding_source_accessor_t;

  std::string show_path(const canon_path_t& path) override {
    return display_prefix + path.abs() + display_suffix;
  }
};

std::pair<ref<source_accessor_t>, input_t> input_t::getAccessorUnchecked(const settings_t& settings,
                                                                         store_t& store) const {
  // FIXME: cache the accessor

  if (!scheme)
    throw Error("cannot fetch unsupported input '%s'", attrs_to_json(toAttrs()));

  std::optional<store_path_t> store_path;
  if (isFinal() && getNarHash())
    store_path = computeStorePath(store);

  auto makeStoreAccessor = [&]() -> std::pair<ref<source_accessor_t>, input_t> {
    auto accessor =
        make_ref<substituted_source_accessor_t>(store.requireStoreObjectAccessor(*store_path));

    // FIXME: use the NAR hash for fingerprinting Git trees that have a .gitattributes file, since
    // we don't know if we used `git archive` or libgit2 to fetch it.
    accessor->fingerprint =
        getType() == "git" && accessor->path_exists(canon_path_t(".gitattributes"))
            ? std::optional(store_path->hash_part())
            : get_fingerprint(store);
    cachedFingerprint = accessor->fingerprint;

    // store_t a cache entry for the substituted tree so later fetches
    // can reuse the existing nar instead of copying the unpacked
    // input back into the store on every evaluation.
    if (accessor->fingerprint) {
      settings.get_cache()->upsert(
          make_source_path_to_hash_cache_key(*accessor->fingerprint,
                                             content_address_method_t::raw_t::nix_archive, "/"),
          {{"hash",
            store.queryPathInfo(*store_path)->nar_hash.to_string(hash_format_t::sri, true)}});
    }

    // FIXME: ideally we would use the `showPath()` of the
    // "real" accessor for this fetcher type.
    accessor->set_path_display("«" + to_string(true) + "»");

    return {accessor, *this};
  };

  /* If a tree with the expected hash is already in the Nix store,
     reuse it. We only do this for final inputs, since otherwise
     there is a risk that we don't return the same attributes (like
     `last_modified`) that the "real" fetcher would return. */
  if (store_path && store.isValidPath(*store_path)) {
    debug("using input '%s' in '%s'", to_string(), store.printStorePath(*store_path));
    return makeStoreAccessor();
  }

  try {
    auto [accessor, result] = scheme->get_accessor(settings, store, *this);

    if (auto fp = accessor->get_fingerprint(canon_path_t::root).second)
      result.cachedFingerprint = *fp;
    else
      accessor->fingerprint = result.get_fingerprint(store);

    return {accessor, std::move(result)};
  } catch (Error& e) {
    if (store_path) {
      // Fall back to substitution.
      try {
        store.ensure_path(*store_path);
        warn("Successfully substituted input '%s' after failing to fetch it from its original "
             "location: %s",
             to_string(), e.info().msg);
        return makeStoreAccessor();
      }
      // Ignore any substitution error, rethrow the original error.
      catch (Error& e2) {
        debug("substitution of input '%s' failed: %s", to_string(), e2.info().msg);
      } catch (...) {
      }
    }
    throw;
  }
}

input_t input_t::applyOverrides(std::optional<std::string> ref, std::optional<Hash> rev) const {
  if (!scheme)
    return *this;
  return scheme->applyOverrides(*this, ref, rev);
}

void input_t::clone(const settings_t& settings, store_t& store,
                    const std::filesystem::path& dest_dir) const {
  assert(scheme);
  scheme->clone(settings, store, *this, dest_dir);
}

std::optional<std::filesystem::path> input_t::get_source_path() const {
  assert(scheme);
  return scheme->get_source_path(*this);
}

void input_t::putFile(const canon_path_t& path, std::string_view contents,
                      std::optional<std::string> commit_msg) const {
  assert(scheme);
  return scheme->putFile(*this, path, contents, commit_msg);
}

std::string input_t::get_name() const {
  return maybe_get_str_attr(attrs, "name").value_or("source");
}

store_path_t input_t::computeStorePath(store_t& store) const {
  auto nar_hash = getNarHash();
  if (!nar_hash)
    throw Error("cannot compute store path for unlocked input '%s'", to_string());
  return store.makeFixedOutputPath(get_name(), FixedOutputInfo{
                                                   .method = file_ingestion_method_t::nix_archive,
                                                   .hash = *nar_hash,
                                                   .references = {},
                                               });
}

std::string input_t::getType() const {
  return get_str_attr(attrs, "type");
}

std::optional<Hash> input_t::getNarHash() const {
  if (auto s = maybe_get_str_attr(attrs, "narHash")) {
    auto hash = s->empty() ? Hash(hash_algorithm_t::SHA256) : Hash::parse_sri(*s);
    if (hash.algo() != hash_algorithm_t::SHA256)
      throw UsageError("narHash must use SHA-256");
    return hash;
  }
  return {};
}

std::optional<std::string> input_t::getRef() const {
  if (auto s = maybe_get_str_attr(attrs, "ref"))
    return *s;
  return {};
}

std::optional<Hash> input_t::getRev() const {
  std::optional<Hash> hash = {};

  if (auto s = maybe_get_str_attr(attrs, "rev")) {
    try {
      hash = Hash::parse_any_prefixed(*s);
    } catch (BadHash& e) {
      // Default to sha1 for backwards compatibility with existing
      // usages (e.g. `builtins.fetchTree` calls or flake inputs).
      hash = Hash::parse_any(*s, hash_algorithm_t::SHA1);
    }
  }

  return hash;
}

std::optional<uint64_t> input_t::get_rev_count() const {
  if (auto n = maybe_get_int_attr(attrs, "revCount"))
    return *n;
  return {};
}

std::optional<time_t> input_t::get_last_modified() const {
  if (auto n = maybe_get_int_attr(attrs, "lastModified"))
    return *n;
  return {};
}

parsed_url_t input_scheme_t::toURL(const input_t& input, bool abbreviate) const {
  throw Error("don't know how to convert input '%s' to a URL", attrs_to_json(input.attrs));
}

input_t input_scheme_t::applyOverrides(const input_t& input, std::optional<std::string> ref,
                                       std::optional<Hash> rev) const {
  if (ref)
    throw Error("don't know how to set branch/tag name of input '%s' to '%s'", input.to_string(),
                *ref);
  if (rev)
    throw Error("don't know how to set revision of input '%s' to '%s'", input.to_string(),
                rev->git_rev());
  return input;
}

std::optional<std::filesystem::path> input_scheme_t::get_source_path(const input_t& input) const {
  return {};
}

void input_scheme_t::putFile(const input_t& input, const canon_path_t& path,
                             std::string_view contents,
                             std::optional<std::string> commit_msg) const {
  throw Error("input '%s' does not support modifying file '%s'", input.to_string(), path);
}

void input_scheme_t::clone(const settings_t& settings, store_t& store, const input_t& input,
                           const std::filesystem::path& dest_dir) const {
  if (std::filesystem::exists(dest_dir))
    throw Error("cannot clone into existing path %s", dest_dir);

  auto [accessor, input2] = get_accessor(settings, store, input);

  activity_t act(*logger, lvl_talkative, act_unknown,
                 fmt("copying '%s' to %s...", input2.to_string(), dest_dir));

  restore_sink_t sink(/*start_fsync=*/false);
  sink.dst_path = dest_dir;
  copy_recursive(*accessor, canon_path_t::root, sink, canon_path_t::root);
}

std::optional<experimental_feature_t> input_scheme_t::experimental_feature() const {
  return {};
}

std::string public_keys_to_string(const std::vector<public_key_t>& public_keys) {
  return ((nlohmann::json)public_keys).dump();
}

} // namespace nix::fetchers

namespace nlohmann {

using namespace nix;

#ifndef DOXYGEN_SKIP

fetchers::public_key_t adl_serializer<fetchers::public_key_t>::from_json(const json& json) {
  fetchers::public_key_t res = {};
  auto& obj = get_object(json);
  if (auto* type = optional_value_at(obj, "type"))
    res.type = get_string(*type);

  res.key = get_string(value_at(obj, "key"));

  return res;
}

void adl_serializer<fetchers::public_key_t>::to_json(json& json, const fetchers::public_key_t& p) {
  json["type"] = p.type;
  json["key"] = p.key;
}

#endif

} // namespace nlohmann
