#pragma once
///@file

#include <functional>

#include <nlohmann/json.hpp>

#include "nix/util/memory-source-accessor.h"

namespace nix {

struct source_t;

/**
 * Return an object that provides access to the contents of a NAR
 * file.
 */
ref<source_accessor_t> make_nar_accessor(std::string&& nar);

ref<source_accessor_t> make_nar_accessor(source_t& source);

/**
 * Create a NAR accessor from a NAR listing (in the format produced by
 * list_nar()). The callback get_nar_bytes(offset, length) is used by the
 * read_file() method of the accessor to get the contents of files
 * inside the NAR.
 */
using get_nar_bytes_t = std::function<std::string(uint64_t, uint64_t)>;

/**
 * The canonical get_nar_bytes_t function for a seekable source_t.
 */
get_nar_bytes_t seekable_get_nar_bytes(const Path& path);

get_nar_bytes_t seekable_get_nar_bytes(descriptor_t fd);

ref<source_accessor_t> make_lazy_nar_accessor(const nlohmann::json& listing,
                                              get_nar_bytes_t get_nar_bytes);

/**
 * Creates a NAR accessor from a given stream and a get_nar_bytes_t getter.
 * @param source Consumed eagerly. References to it are not persisted in the resulting
 * source_accessor_t.
 */
ref<source_accessor_t> make_lazy_nar_accessor(source_t& source, get_nar_bytes_t get_nar_bytes);

struct nar_listing_regular_file_t {
  /**
   * @see `source_accessor_t::stat_t::file_size`
   */
  std::optional<uint64_t> file_size;

  /**
   * @see `source_accessor_t::stat_t::nar_offset`
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
nar_listing_t list_nar_deep(source_accessor_t& accessor, const canon_path_t& path);

/**
 * Return a shallow structured representation of the contents of a NAR (except file
 * contents), only listing immediate children without recursing.
 */
shallow_nar_listing_t list_nar_shallow(source_accessor_t& accessor, const canon_path_t& path);

// JSON serializers for nar_listing_regular_file_t needed by
// fso::variant_t<nar_listing_regular_file_t, ...>

} // namespace nix

// JSON serializer for nar_listing_regular_file_t
namespace nlohmann {
template <>
struct adl_serializer<nix::nar_listing_regular_file_t> {
  static nix::nar_listing_regular_file_t from_json(const json& j) {
    nix::nar_listing_regular_file_t r;
    if (j.contains("fileSize")) {
      r.file_size = j["fileSize"].get<uint64_t>();
    }
    if (j.contains("narOffset")) {
      r.nar_offset = j["narOffset"].get<uint64_t>();
    }
    return r;
  }
  static void to_json(json& j, const nix::nar_listing_regular_file_t& r) {
    j = json::object();
    if (r.file_size) {
      j["fileSize"] = *r.file_size;
    }
    if (r.nar_offset) {
      j["narOffset"] = *r.nar_offset;
    }
  }
};
} // namespace nlohmann
