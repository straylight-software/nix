#pragma once
///@file

#include <variant>

#include "nix/store/path.h"
#include "nix/util/file-content-address.h"
#include "nix/util/hash.h"
#include "nix/util/json-impls.h"
#include "nix/util/variant-wrapper.h"

namespace nix {

/*
 * Content addressing method
 */

/**
 * Compute the prefix to the hash algorithm which indicates how the
 * files were ingested.
 */
std::string_view make_file_ingestion_prefix(file_ingestion_method_t m);

/**
 * An enumeration of all the ways we can content-address store objects.
 *
 * Just the type of a content address. Combine with the hash itself, and
 * we have a `content_address_t` as defined below. Combine that, in turn,
 * with info on references, and we have `ContentAddressWithReferences`,
 * as defined further below.
 */
struct content_address_method_t {
  enum struct raw_t {
    /**
     * Calculate a store path using the `file_ingestion_method_t::flat`
     * hash of the file system objects, and references.
     *
     * See `store-object/content-address.md#method-flat` in the
     * manual.
     */
    flat,

    /**
     * Calculate a store path using the
     * `file_ingestion_method_t::nix_archive` hash of the file system
     * objects, and references.
     *
     * See `store-object/content-address.md#method-flat` in the
     * manual.
     */
    nix_archive,

    /**
     * Calculate a store path using the `file_ingestion_method_t::git`
     * hash of the file system objects, and references.
     *
     * Part of `experimental_feature_t::git_hashing`.
     *
     * See `store-object/content-address.md#method-git` in the
     * manual.
     */
    git,

    /**
     * Calculate a store path using the `file_ingestion_method_t::flat`
     * hash of the file system objects, and references, but in a
     * different way than `content_address_method_t::raw_t::flat`.
     *
     * See `store-object/content-address.md#method-text` in the
     * manual.
     */
    Text,
  };

  raw_t raw;

  bool operator==(const content_address_method_t&) const = default;
  auto operator<=>(const content_address_method_t&) const = default;

  MAKE_WRAPPER_CONSTRUCTOR(content_address_method_t);

  /**
   * Parse a content addressing method (name).
   *
   * The inverse of `render`.
   */
  static content_address_method_t parse(std::string_view rawCaMethod);

  /**
   * Render a content addressing method (name).
   *
   * The inverse of `parse`.
   */
  std::string_view render() const;

  /**
   * Parse the prefix tag which indicates how the files
   * were ingested, with the fixed output case not prefixed for back
   * compat.
   *
   * @param m A string that should begin with the
   * prefix. On return, the remainder of the string after the
   * prefix.
   */
  static content_address_method_t parsePrefix(std::string_view& m);

  /**
   * Render the prefix tag which indicates how the files wre ingested.
   *
   * The rough inverse of `parsePrefix()`.
   */
  std::string_view renderPrefix() const;

  /**
   * Parse a content addressing method and hash algorithm.
   */
  static std::pair<content_address_method_t, hash_algorithm_t>
  parseWithAlgo(std::string_view rawCaMethod);

  /**
   * Render a content addressing method and hash algorithm in a
   * nicer way, prefixing both cases.
   *
   * The rough inverse of `parse()`.
   */
  std::string renderWithAlgo(hash_algorithm_t ha) const;

  /**
   * Get the underlying way to content-address file system objects.
   *
   * Different ways of hashing store objects may use the same method
   * for hashing file systeme objects.
   */
  file_ingestion_method_t getFileIngestionMethod() const;
};

/*
 * Mini content address
 */

/**
 * We've accumulated several types of content-addressed paths over the
 * years; fixed-output derivations support multiple hash algorithms and
 * serialisation methods (flat file vs NAR). Thus, `ca` has one of the
 * following forms:
 *
 * - `TextIngestionMethod`:
 *   `text:sha256:<sha256 hash of file contents>`
 *
 * - `FixedIngestionMethod`:
 *   `fixed:<r?>:<hash algorithm>:<hash of file contents>`
 */
struct content_address_t {
  /**
   * How the file system objects are serialized
   */
  content_address_method_t method;

  /**
   * Hash of that serialization
   */
  Hash hash;

  bool operator==(const content_address_t&) const = default;
  auto operator<=>(const content_address_t&) const = default;

  /**
   * Compute the content-addressability assertion
   * (`valid_path_info_t::ca`) for paths created by
   * `store_t::makeFixedOutputPath()` / `store_t::add_to_store()`.
   */
  std::string render() const;

  static content_address_t parse(std::string_view rawCa);

  static std::optional<content_address_t> parseOpt(std::string_view rawCaOpt);

  std::string printMethodAlgo() const;
};

/**
 * Render the `content_address_t` if it exists to a string, return empty
 * string otherwise.
 */
std::string render_content_address(std::optional<content_address_t> ca);

/*
 * full content address
 *
 * See the schema for store paths in store-api.cc
 */

/**
 * A set of references to other store objects.
 *
 * References to other store objects are tracked with store paths, self
 * references however are tracked with a boolean.
 */
struct store_references_t {
  /**
   * References to other store objects
   */
  store_path_set_t others;

  /**
   * Reference to this store object
   */
  bool self = false;

  /**
   * @return true iff no references, i.e. others is empty and self is
   * false.
   */
  bool empty() const;

  /**
   * Returns the numbers of references, i.e. the size of others + 1
   * iff self is true.
   */
  size_t size() const;

  bool operator==(const store_references_t&) const = default;
  // TODO libc++ 16 (used by darwin) missing `std::map::operator <=>`, can't do yet.
  // auto operator <=>(const store_references_t &) const = default;
};

// This matches the additional info that we need for makeTextPath
struct TextInfo {
  /**
   * Hash of the contents of the text/file.
   */
  Hash hash;

  /**
   * References to other store objects only; self references
   * disallowed
   */
  store_path_set_t references;

  bool operator==(const TextInfo&) const = default;
  // TODO libc++ 16 (used by darwin) missing `std::map::operator <=>`, can't do yet.
  // auto operator <=>(const TextInfo &) const = default;
};

struct FixedOutputInfo {
  /**
   * How the file system objects are serialized
   */
  file_ingestion_method_t method;

  /**
   * Hash of that serialization
   */
  Hash hash;

  /**
   * References to other store objects or this one.
   */
  store_references_t references;

  bool operator==(const FixedOutputInfo&) const = default;
  // TODO libc++ 16 (used by darwin) missing `std::map::operator <=>`, can't do yet.
  // auto operator <=>(const FixedOutputInfo &) const = default;
};

/**
 * Ways of content addressing but not a complete content_address_t.
 *
 * A content_address_t without a Hash.
 */
struct ContentAddressWithReferences {
  typedef std::variant<TextInfo, FixedOutputInfo> raw_t;

  raw_t raw;

  bool operator==(const ContentAddressWithReferences&) const = default;
  // TODO libc++ 16 (used by darwin) missing `std::map::operator <=>`, can't do yet.
  // auto operator <=>(const ContentAddressWithReferences &) const = default;

  MAKE_WRAPPER_CONSTRUCTOR(ContentAddressWithReferences);

  /**
   * Create a `ContentAddressWithReferences` from a mere
   * `content_address_t`, by claiming no references.
   */
  static ContentAddressWithReferences withoutRefs(const content_address_t&) noexcept;

  /**
   * Create a `ContentAddressWithReferences` from 3 parts:
   *
   * @param method Way ingesting the file system data.
   *
   * @param hash Hash of ingested file system data.
   *
   * @param refs References to other store objects or oneself.
   *
   * @note note that all combinations are supported. This is a
   * *partial function* and exceptions will be thrown for invalid
   * combinations.
   */
  static ContentAddressWithReferences fromParts(content_address_method_t method, Hash hash,
                                                store_references_t refs);

  content_address_method_t getMethod() const;

  Hash getHash() const;
};

template <>
struct json_avoids_null<content_address_method_t> : std::true_type {};

template <>
struct json_avoids_null<content_address_t> : std::true_type {};

} // namespace nix

JSON_IMPL(nix::content_address_method_t)
JSON_IMPL(nix::content_address_t)
