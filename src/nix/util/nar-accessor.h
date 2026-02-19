#pragma once
///@file

#include <functional>

#include <nlohmann/json_fwd.hpp>

#include "nix/util/memory-source-accessor.h"

namespace nix {

struct Source;

/**
 * Return an object that provides access to the contents of a NAR
 * file.
 */
ref<SourceAccessor> makeNarAccessor(std::string&& nar);

ref<SourceAccessor> makeNarAccessor(Source& source);

/**
 * Create a NAR accessor from a NAR listing (in the format produced by
 * listNar()). The callback getNarBytes(offset, length) is used by the
 * readFile() method of the accessor to get the contents of files
 * inside the NAR.
 */
using get_nar_bytes_t = std::function<std::string(uint64_t, uint64_t)>;

/**
 * The canonical get_nar_bytes_t function for a seekable Source.
 */
get_nar_bytes_t seekableGetNarBytes(const Path& path);

get_nar_bytes_t seekableGetNarBytes(descriptor_t fd);

ref<SourceAccessor> makeLazyNarAccessor(const nlohmann::json& listing, get_nar_bytes_t getNarBytes);

/**
 * Creates a NAR accessor from a given stream and a get_nar_bytes_t getter.
 * @param source Consumed eagerly. References to it are not persisted in the resulting
 * SourceAccessor.
 */
ref<SourceAccessor> makeLazyNarAccessor(Source& source, get_nar_bytes_t getNarBytes);

struct nar_listing_regular_file_t {
  /**
   * @see `SourceAccessor::stat_t::fileSize`
   */
  std::optional<uint64_t> fileSize;

  /**
   * @see `SourceAccessor::stat_t::narOffset`
   *
   * We only set to non-`std::nullopt` if it is also non-zero.
   */
  std::optional<uint64_t> narOffset;

  auto operator<=>(const nar_listing_regular_file_t&) const = default;
};

/**
 * Abstract syntax for a "NAR listing".
 */
using nar_listing_t = fso::variant_t<nar_listing_regular_file_t, true>;

/**
 * Shallow NAR listing where directory children are not recursively expanded.
 * Uses a variant that can hold Regular/Symlink fully, but directory_t children
 * are just unit types indicating presence without content.
 */
using shallow_nar_listing_t = fso::variant_t<nar_listing_regular_file_t, false>;

/**
 * Return a deep structured representation of the contents of a NAR (except file
 * contents), recursively listing all children.
 */
nar_listing_t listNarDeep(SourceAccessor& accessor, const canon_path_t& path);

/**
 * Return a shallow structured representation of the contents of a NAR (except file
 * contents), only listing immediate children without recursing.
 */
shallow_nar_listing_t listNarShallow(SourceAccessor& accessor, const canon_path_t& path);

// All json_avoids_null and JSON_IMPL covered by generic templates in memory-source-accessor.hh

} // namespace nix
