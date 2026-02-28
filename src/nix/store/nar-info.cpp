#include "nix/store/nar-info.h"

#include "cornell/nix/nix_formats.h"
#include "nix/store/globals.h"
#include "nix/store/store-api.h"
#include "nix/util/json-utils.h"
#include "nix/util/strings.h"

namespace nix {

// ═══════════════════════════════════════════════════════════════════════════════
// Cornell Conversion (Checkpoint 3)
// Convert verified Cornell narinfo_t to legacy nar_info_t
// ═══════════════════════════════════════════════════════════════════════════════

[[nodiscard]] auto from_cornell_narinfo(const store_dir_config_t& store,
                                        const cornell::nix::narinfo_t& cn) -> nar_info_t {
  // Parse store path
  auto path = store.parseStorePath(cn.store_path);

  // Parse nar_hash (required field)
  auto nar_hash = Hash::parse_any_prefixed(cn.nar_hash);

  // Construct base nar_info_t
  nar_info_t info(store, std::move(path), nar_hash);

  // URL and compression
  info.url = cn.url;
  info.compression = std::string(cornell::nix::compression_to_string(cn.compression));
  if (info.compression.empty()) {
    info.compression = "bzip2"; // default per legacy parser
  }

  // File hash (optional)
  if (cn.file_hash) {
    info.fileHash = Hash::parse_any_prefixed(*cn.file_hash);
  }

  // Sizes
  info.file_size = cn.file_size;
  info.nar_size = cn.nar_size;

  // References
  for (const auto& ref : cn.references) {
    info.references.insert(store_path_t(ref));
  }

  // Deriver (optional)
  if (cn.deriver && *cn.deriver != "unknown-deriver") {
    info.deriver = store_path_t(*cn.deriver);
  }

  // Signatures
  for (const auto& sig : cn.sigs) {
    info.sigs.insert(sig.key_name + ":" + sig.sig);
  }

  // Content address (optional)
  if (cn.ca) {
    info.ca = content_address_t::parseOpt(*cn.ca);
  }

  return info;
}

nar_info_t::nar_info_t(const store_dir_config_t& store, const std::string& s,
                       const std::string& whence)
    : UnkeyedValidPathInfo(store, Hash::dummy) // FIXME: hack
      ,
      valid_path_info_t(store_path_t::dummy,
                        static_cast<const UnkeyedValidPathInfo&>(*this)) // FIXME: hack
      ,
      UnkeyedNarInfo(static_cast<const UnkeyedValidPathInfo&>(*this)) {
  unsigned line = 1;

  auto corrupt = [&](const char* reason) {
    return Error("NAR info file '%1%' is corrupt: %2%", whence,
                 std::string(reason) + (line > 0 ? " at line " + std::to_string(line) : ""));
  };

  auto parseHashField = [&](const std::string& s) {
    try {
      return Hash::parse_any_prefixed(s);
    } catch (BadHash&) {
      throw corrupt("bad hash");
    }
  };

  bool havePath = false;
  bool haveNarHash = false;

  size_t pos = 0;
  while (pos < s.size()) {
    size_t colon = s.find(':', pos);
    if (colon == s.npos)
      throw corrupt("expecting ':'");

    std::string name(s, pos, colon - pos);

    // Validate format: "Key: Value\n" - must have space after colon
    // colon + 1 is where space should be, colon + 2 is where value starts
    if (colon + 1 >= s.size())
      throw corrupt("unexpected end of input after ':'");

    if (s[colon + 1] != ' ')
      throw corrupt("expecting space after ':'");

    size_t value_start = colon + 2;

    // Find end of line - search from colon+1 to handle empty values
    size_t eol = s.find('\n', colon + 1);
    if (eol == s.npos)
      throw corrupt("expecting '\\n' (missing newline at end of line)");

    // Handle CRLF line endings by stripping trailing CR
    size_t value_end = eol;
    if (value_end > value_start && s[value_end - 1] == '\r')
      value_end--;

    // Extract value (may be empty if value_start >= value_end)
    std::string value;
    if (value_start < value_end)
      value = std::string(s, value_start, value_end - value_start);
    // else: empty value is allowed

    if (name == "store_path_t") {
      path = store.parseStorePath(value);
      havePath = true;
    } else if (name == "URL")
      url = value;
    else if (name == "Compression")
      compression = value;
    else if (name == "FileHash")
      fileHash = parseHashField(value);
    else if (name == "FileSize") {
      auto n = string2_int<decltype(file_size)>(value);
      if (!n)
        throw corrupt("invalid FileSize");
      file_size = *n;
    } else if (name == "NarHash") {
      nar_hash = parseHashField(value);
      haveNarHash = true;
    } else if (name == "NarSize") {
      auto n = string2_int<decltype(nar_size)>(value);
      if (!n)
        throw corrupt("invalid NarSize");
      nar_size = *n;
    } else if (name == "References") {
      auto refs = tokenize_string<strings_t>(value, " ");
      if (!references.empty())
        throw corrupt("extra References");
      for (auto& r : refs)
        references.insert(store_path_t(r));
    } else if (name == "Deriver") {
      if (value != "unknown-deriver")
        deriver = store_path_t(value);
    } else if (name == "Sig")
      sigs.insert(value);
    else if (name == "CA") {
      if (ca)
        throw corrupt("extra CA");
      // FIXME: allow blank ca or require skipping field?
      ca = content_address_t::parseOpt(value);
    }

    pos = eol + 1;
    line += 1;
  }

  if (compression == "")
    compression = "bzip2";

  if (!havePath || !haveNarHash || url.empty() || nar_size == 0) {
    line = 0; // don't include line information in the error
    throw corrupt(!havePath       ? "store_path_t missing"
                  : !haveNarHash  ? "NarHash missing"
                  : url.empty()   ? "URL missing"
                  : nar_size == 0 ? "NarSize missing or zero"
                                  : "?");
  }
}

std::string nar_info_t::to_string(const store_dir_config_t& store) const {
  std::string res;
  res += "store_path_t: " + store.printStorePath(path) + "\n";
  res += "URL: " + url + "\n";
  assert(compression != "");
  res += "Compression: " + compression + "\n";
  assert(fileHash && fileHash->algo() == hash_algorithm_t::SHA256);
  res += "FileHash: " + fileHash->to_string(hash_format_t::nix32, true) + "\n";
  res += "FileSize: " + std::to_string(file_size) + "\n";
  assert(nar_hash.algo() == hash_algorithm_t::SHA256);
  res += "NarHash: " + nar_hash.to_string(hash_format_t::nix32, true) + "\n";
  res += "NarSize: " + std::to_string(nar_size) + "\n";

  res += "References: " + concat_strings_sep(" ", shortRefs()) + "\n";

  if (deriver)
    res += "Deriver: " + std::string(deriver->to_string()) + "\n";

  for (const auto& sig : sigs)
    res += "Sig: " + sig + "\n";

  if (ca)
    res += "CA: " + render_content_address(*ca) + "\n";

  return res;
}

nlohmann::json UnkeyedNarInfo::to_json(const store_dir_config_t* store, bool includeImpureInfo,
                                       PathInfoJsonFormat format) const {
  using nlohmann::json;

  auto json_object = UnkeyedValidPathInfo::to_json(store, includeImpureInfo, format);

  if (includeImpureInfo) {
    if (!url.empty())
      json_object["url"] = url;
    if (!compression.empty())
      json_object["compression"] = compression;
    if (fileHash) {
      if (format == PathInfoJsonFormat::V1)
        json_object["downloadHash"] = fileHash->to_string(hash_format_t::sri, true);
      else
        json_object["downloadHash"] = *fileHash;
    }
    if (file_size)
      json_object["downloadSize"] = file_size;
  }

  return json_object;
}

UnkeyedNarInfo UnkeyedNarInfo::from_json(const store_dir_config_t* store,
                                         const nlohmann::json& json) {
  UnkeyedNarInfo res{UnkeyedValidPathInfo::from_json(store, json)};

  auto& obj = get_object(json);

  PathInfoJsonFormat format = PathInfoJsonFormat::V1;
  if (auto* version = optional_value_at(obj, "version"))
    format = *version;

  if (auto* url = get(obj, "url"))
    res.url = get_string(*url);

  if (auto* compression = get(obj, "compression"))
    res.compression = get_string(*compression);

  if (auto* downloadHash = get(obj, "downloadHash")) {
    if (format == PathInfoJsonFormat::V1)
      res.fileHash = Hash::parse_sri(get_string(*downloadHash));
    else
      res.fileHash = *downloadHash;
  }

  if (auto* downloadSize = get(obj, "downloadSize"))
    res.file_size = get_unsigned(*downloadSize);

  return res;
}

} // namespace nix

namespace nlohmann {

nix::UnkeyedNarInfo adl_serializer<nix::UnkeyedNarInfo>::from_json(const json& json) {
  return nix::UnkeyedNarInfo::from_json(nullptr, json);
}

void adl_serializer<nix::UnkeyedNarInfo>::to_json(json& json, const nix::UnkeyedNarInfo& c) {
  json = c.to_json(nullptr, true, nix::PathInfoJsonFormat::V2);
}

} // namespace nlohmann
