#include <array>

#include <nlohmann/json.hpp>

#include "nix/store/store-dir-config.h"
#include "nix/util/json-utils.h"

namespace nix {

// Lookup table for O(1) store path name character validation.
// Valid characters: a-z A-Z 0-9 + - . _ ? =
static constexpr std::array<bool, 256> valid_name_chars = []() {
  std::array<bool, 256> table{};
  for (unsigned char c = '0'; c <= '9'; ++c) {
    table[c] = true;
  }
  for (unsigned char c = 'a'; c <= 'z'; ++c) {
    table[c] = true;
  }
  for (unsigned char c = 'A'; c <= 'Z'; ++c) {
    table[c] = true;
  }
  for (unsigned char c : {'+', '-', '.', '_', '?', '='}) {
    table[c] = true;
  }
  return table;
}();

void check_name(std::string_view name) {
  if (name.empty()) {
    throw BadStorePathName("name must not be empty");
  }
  if (name.size() > store_path_t::MaxPathLen) {
    throw BadStorePathName("name '%s' must be no longer than %d characters", name,
                           store_path_t::MaxPathLen);
  }
  // See nameRegexStr for the definition
  if (name[0] == '.') {
    // check against "." and "..", followed by end or dash
    if (name.size() == 1) {
      throw BadStorePathName("name '%s' is not valid", name);
    }
    if (name[1] == '-') {
      throw BadStorePathName(
          "name '%s' is not valid: first dash-separated component must not be '%s'", name, ".");
    }
    if (name[1] == '.') {
      if (name.size() == 2) {
        throw BadStorePathName("name '%s' is not valid", name);
      }
      if (name[2] == '-') {
        throw BadStorePathName(
            "name '%s' is not valid: first dash-separated component must not be '%s'", name, "..");
      }
    }
  }
  for (auto c : name) {
    if (!valid_name_chars[static_cast<unsigned char>(c)]) {
      throw BadStorePathName("name '%s' contains illegal character '%s'", name, c);
    }
  }
}

static void check_path_name(std::string_view path, std::string_view name) {
  try {
    check_name(name);
  } catch (BadStorePathName& e) {
    throw BadStorePath("path '%s' is not a valid store path: %s", path, uncolored_t(e.message()));
  }
}

store_path_t::store_path_t(std::string_view _baseName) : base_name(_baseName) {
  if (base_name.size() < HashLen + 1) {
    throw BadStorePath("'%s' is too short to be a valid store path", base_name);
  }
  for (auto c : hash_part()) {
    if (c == 'e' || c == 'o' || c == 'u' || c == 't' ||
        !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z'))) {
      throw BadStorePath("store path '%s' contains illegal base-32 character '%s'", base_name, c);
    }
  }
  check_path_name(base_name, name());
}

store_path_t::store_path_t(const Hash& hash, std::string_view _name)
    : base_name((hash.to_string(hash_format_t::nix32, false) + "-").append(std::string(_name))) {
  check_path_name(base_name, name());
}

bool store_path_t::is_derivation() const noexcept {
  return has_suffix(name(), drvExtension);
}

void store_path_t::requireDerivation() const {
  if (!is_derivation()) {
    throw FormatError("store path '%s' is not a valid derivation path", to_string());
  }
}

store_path_t store_path_t::dummy("ffffffffffffffffffffffffffffffff-x");

store_path_t store_path_t::random(std::string_view name) {
  return store_path_t(Hash::random(hash_algorithm_t::SHA1), name);
}

} // namespace nix

namespace nlohmann {

nix::store_path_t adl_serializer<nix::store_path_t>::from_json(const json& json) {
  return nix::store_path_t{nix::get_string(json)};
}

void adl_serializer<nix::store_path_t>::to_json(json& json, const nix::store_path_t& store_path) {
  json = store_path.to_string();
}

} // namespace nlohmann
