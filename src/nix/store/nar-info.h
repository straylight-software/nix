#pragma once
///@file

#include "nix/store/path-info.h"
#include "nix/util/hash.h"
#include "nix/util/types.h"

// Forward declaration for Cornell verified narinfo type
namespace cornell::nix {
struct narinfo_t;
}

namespace nix {

struct store_dir_config_t;

struct UnkeyedNarInfo : virtual UnkeyedValidPathInfo {
  std::string url;
  std::string compression;
  std::optional<Hash> fileHash;
  uint64_t file_size = 0;

  UnkeyedNarInfo(UnkeyedValidPathInfo info) : UnkeyedValidPathInfo(std::move(info)) {}

  bool operator==(const UnkeyedNarInfo&) const = default;
  // TODO libc++ 16 (used by darwin) missing `std::optional::operator <=>`, can't do yet
  // auto operator <=>(const nar_info_t &) const = default;

  nlohmann::json to_json(const store_dir_config_t* store, bool includeImpureInfo,
                         PathInfoJsonFormat format) const override;
  static UnkeyedNarInfo from_json(const store_dir_config_t* store, const nlohmann::json& json);
};

/**
 * Key and the extra NAR fields
 */
struct nar_info_t : valid_path_info_t, UnkeyedNarInfo {
  nar_info_t() = delete;

  nar_info_t(valid_path_info_t info)
      : UnkeyedValidPathInfo{static_cast<UnkeyedValidPathInfo&&>(info)}
        /* Later copies from `*this` are pointless. The argument is only
           there so the constructors can also call
           `UnkeyedValidPathInfo`, but this won't happen since the base
           class is virtual. Only this counstructor (assuming it is most
           derived) will initialize that virtual base class. */
        ,
        valid_path_info_t{info.path, static_cast<const UnkeyedValidPathInfo&>(*this)},
        UnkeyedNarInfo{static_cast<const UnkeyedValidPathInfo&>(*this)} {}

  nar_info_t(const store_dir_config_t& store, store_path_t path, Hash nar_hash)
      : nar_info_t{valid_path_info_t{std::move(path), UnkeyedValidPathInfo{store, nar_hash}}} {}

  nar_info_t(std::string store_dir, store_path_t path, Hash nar_hash)
      : nar_info_t{valid_path_info_t{std::move(path),
                                     UnkeyedValidPathInfo{std::move(store_dir), nar_hash}}} {}

  static nar_info_t makeFromCA(const store_dir_config_t& store, std::string_view name,
                               ContentAddressWithReferences ca, Hash nar_hash) {
    return valid_path_info_t::makeFromCA(store, std::move(name), std::move(ca), nar_hash);
  }

  nar_info_t(const store_dir_config_t& store, const std::string& s, const std::string& whence);

  bool operator==(const nar_info_t&) const = default;

  std::string to_string(const store_dir_config_t& store) const;
};

// ═══════════════════════════════════════════════════════════════════════════════
// Cornell Conversion (Checkpoint 3)
// ═══════════════════════════════════════════════════════════════════════════════

/**
 * Convert Cornell verified narinfo to legacy nar_info_t.
 * Used for shadow-then-flip pattern: Cornell parses, we convert, legacy shadows.
 */
[[nodiscard]] auto from_cornell_narinfo(const store_dir_config_t& store,
                                        const cornell::nix::narinfo_t& cn) -> nar_info_t;

} // namespace nix

JSON_IMPL(nix::UnkeyedNarInfo)
