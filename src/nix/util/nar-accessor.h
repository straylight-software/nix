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
ref<SourceAccessor> make_nar_accessor(std::string&& nar);

ref<SourceAccessor> make_nar_accessor(Source& source);

/**
 * Create a NAR accessor from a NAR listing (in the format produced by
 * list_nar()). The callback get_nar_bytes(offset, length) is used by the
 * read_file() method of the accessor to get the contents of files
 * inside the NAR.
 */
using get_nar_bytes_t = std::function<std::string(uint64_t, uint64_t)>;

/**
 * The canonical get_nar_bytes_t function for a seekable Source.
 */
get_nar_bytes_t seekable_get_nar_bytes(const Path& path);

get_nar_bytes_t seekable_get_nar_bytes(descriptor_t fd);

ref<SourceAccessor> make_lazy_nar_accessor(const nlohmann::json& listing, get_nar_bytes_t get_nar_bytes);

/**
 * Creates a NAR accessor from a given stream and a get_nar_bytes_t getter.
 * @param source Consumed eagerly. References to it are not persisted in the resulting
 * SourceAccessor.
 */
ref<SourceAccessor> make_lazy_nar_accessor(Source& source, get_nar_bytes_t get_nar_bytes);

struct nar_listing_regular_file_t {
  /**
   * @see `SourceAccessor::stat_t::file_size`
   */
  std::optional<uint64_t> file_size;

  /**
   * @see `SourceAccessor::stat_t::nar_offset`
   *
   * We only set to non-`std::nullopt` if it is also non-zero.
   */
  std::optional<uint64_t> nar_offset;

  auto operator<=>(const nar_listing_regular_file_t&) const = default;
};

/**
 * Abstract syntax for a "NAR listing".
 */
using nar_listing_t = fso::variant_t<nar_listing_regular_file_t, true>;

/**
 * Shallow NAR listing where directory children are not recursively expanded.
 * Uses a variant that can hold regular/symlink fully, but directory_t children
 * are just unit types indicating presence without content.
 */
using shallow_nar_listing_t = fso::variant_t<nar_listing_regular_file_t, false>;

/**
 * Return a deep structured representation of the contents of a NAR (except file
 * contents), recursively listing all children.
 */
nar_listing_t list_nar_deep(SourceAccessor& accessor, const canon_path_t& path);

/**
 * Return a shallow structured representation of the contents of a NAR (except file
 * contents), only listing immediate children without recursing.
 */
shallow_nar_listing_t list_nar_shallow(SourceAccessor& accessor, const canon_path_t& path);

// All json_avoids_null and JSON_IMPL covered by generic templates in memory-source-accessor.hh

} // namespace nix
