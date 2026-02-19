#pragma once
///@file

#include "nix/store/path-info.h"
#include "nix/util/hash.h"
#include "nix/util/types.h"

namespace nix {

struct StoreDirConfig;

struct UnkeyedNarInfo : virtual UnkeyedValidPathInfo {
  std::string url;
  std::string compression;
  std::optional<Hash> fileHash;
  uint64_t file_size = 0;

  UnkeyedNarInfo(UnkeyedValidPathInfo info) : UnkeyedValidPathInfo(std::move(info)) {}

  bool operator==(const UnkeyedNarInfo&) const = default;
  // TODO libc++ 16 (used by darwin) missing `std::optional::operator <=>`, can't do yet
  // auto operator <=>(const NarInfo &) const = default;

  nlohmann::json to_json(const StoreDirConfig* store, bool includeImpureInfo,
                        PathInfoJsonFormat format) const override;
  static UnkeyedNarInfo from_json(const StoreDirConfig* store, const nlohmann::json& json);
};

/**
 * Key and the extra NAR fields
 */
struct NarInfo : ValidPathInfo, UnkeyedNarInfo {
  NarInfo() = delete;

  NarInfo(ValidPathInfo info)
      : UnkeyedValidPathInfo{static_cast<UnkeyedValidPathInfo&&>(info)}
        /* Later copies from `*this` are pointless. The argument is only
           there so the constructors can also call
           `UnkeyedValidPathInfo`, but this won't happen since the base
           class is virtual. Only this counstructor (assuming it is most
           derived) will initialize that virtual base class. */
        ,
        ValidPathInfo{info.path, static_cast<const UnkeyedValidPathInfo&>(*this)},
        UnkeyedNarInfo{static_cast<const UnkeyedValidPathInfo&>(*this)} {}

  NarInfo(const StoreDirConfig& store, StorePath path, Hash nar_hash)
      : NarInfo{ValidPathInfo{std::move(path), UnkeyedValidPathInfo{store, nar_hash}}} {}

  NarInfo(std::string store_dir, StorePath path, Hash nar_hash)
      : NarInfo{
            ValidPathInfo{std::move(path), UnkeyedValidPathInfo{std::move(store_dir), nar_hash}}} {}

  static NarInfo makeFromCA(const StoreDirConfig& store, std::string_view name,
                            ContentAddressWithReferences ca, Hash nar_hash) {
    return ValidPathInfo::makeFromCA(store, std::move(name), std::move(ca), nar_hash);
  }

  NarInfo(const StoreDirConfig& store, const std::string& s, const std::string& whence);

  bool operator==(const NarInfo&) const = default;

  std::string to_string(const StoreDirConfig& store) const;
};

} // namespace nix

JSON_IMPL(nix::UnkeyedNarInfo)
