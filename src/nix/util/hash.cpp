#include "nix/util/hash.h"

#include <cstring>
#include <iostream>

#include <blake3.h>
#include <fcntl.h>
#include <openssl/crypto.h>
#include <openssl/md5.h>
#include <openssl/sha.h>
#include <sodium.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "nix/util/archive.h"
#include "nix/util/args.h"
#include "nix/util/base-n.h"
#include "nix/util/base-nix-32.h"
#include "nix/util/configuration.h"
#include "nix/util/json-utils.h"
#include "nix/util/split.h"

namespace nix {

const string_set_t hash_algorithms = {"blake3", "md5", "sha1", "sha256", "sha512"};

const string_set_t hash_formats = {"base64", "nix32", "base16", "sri"};

Hash::Hash(hash_algorithm_t algo, const experimental_feature_settings_t& xp_settings) : algo(algo) {
  if (algo == hash_algorithm_t::BLAKE3) {
    xp_settings.require(xp_t::blak_e3_hashes);
  }
  hash_size = regular_hash_size(algo);
  assert(hash_size <= max_hash_size);
  memset(hash, 0, max_hash_size);
}

bool Hash::operator==(const Hash& h2) const noexcept {
  if (hash_size != h2.hash_size) {
    return false;
}
  for (unsigned int i = 0; i < hash_size; i++) {
    if (hash[i] != h2.hash[i]) {
      return false;
}
}
  return true;
}

std::strong_ordering Hash::operator<=>(const Hash& h) const noexcept {
  if (auto cmp = hash_size <=> h.hash_size; cmp != 0) {
    return cmp;
}
  for (unsigned int i = 0; i < hash_size; i++) {
    if (auto cmp = hash[i] <=> h.hash[i]; cmp != 0) {
      return cmp;
}
  }
  if (auto cmp = algo <=> h.algo; cmp != 0) {
    return cmp;
}
  return std::strong_ordering::equivalent;
}

std::string Hash::to_string(hash_format_t hash_format, bool include_algo) const {
  std::string s;
  if (hash_format == hash_format_t::SRI || include_algo) {
    s += print_hash_algo(algo);
    s += hash_format == hash_format_t::SRI ? '-' : ':';
  }
  const auto bytes = std::as_bytes(std::span<const uint8_t>{&hash[0], hash_size});
  switch (hash_format) {
    case hash_format_t::base16:
      assert(hash_size);
      s += base16::encode(bytes);
      break;
    case hash_format_t::nix32:
      assert(hash_size);
      s += base_nix32_t::encode(bytes);
      break;
    case hash_format_t::base64:
    case hash_format_t::SRI:
      assert(hash_size);
      s += base64::encode(bytes);
      break;
  }
  return s;
}

Hash Hash::dummy(hash_algorithm_t::SHA256);

namespace {

/// Private convenience
struct decode_name_pair_t {
  decltype(base16::decode)* decode;
  std::string_view encoding_name;
};

} // namespace

static decode_name_pair_t base_explicit(hash_format_t format) {
  switch (format) {
    case hash_format_t::base16:
      return {base16::decode, "base16"};
    case hash_format_t::nix32:
      return {base_nix32_t::decode, "nix32"};
    case hash_format_t::base64:
      return {base64::decode, "Base64"};
    case hash_format_t::SRI:
      break;
  }
  unreachable();
}

/**
 * Given the expected size of the message once decoded it, figure out
 * which encoding we are using by looking at the size of the encoded
 * message.
 */
static hash_format_t base_from_size(std::string_view rest, hash_algorithm_t algo) {
  auto hash_size = regular_hash_size(algo);

  if (rest.size() == base16::encoded_length(hash_size)) {
    return hash_format_t::base16;
}

  if (rest.size() == base_nix32_t::encoded_length(hash_size)) {
    return hash_format_t::nix32;
}

  if (rest.size() == base64::encoded_length(hash_size)) {
    return hash_format_t::base64;
}

  throw BadHash("hash '%s' has wrong length for hash algorithm '%s'", rest, print_hash_algo(algo));
}

/**
 * Given the exact decoding function, and a display name for in error
 * messages.
 *
 * @param rest the string view to parse. Must not include any `<algo>(:|-)` prefix.
 */
static Hash
parse_low_level(std::string_view rest, hash_algorithm_t algo, decode_name_pair_t pair,
              const experimental_feature_settings_t& xp_settings = experimental_feature_settings) {
  Hash res{algo, xp_settings};
  std::string d;
  try {
    d = pair.decode(rest);
  } catch (Error& e) {
    e.add_trace({}, "While decoding hash '%s'", rest);
  }
  if (d.size() != res.hash_size) {
    throw BadHash("invalid %s hash '%s', length %d != expected length %d", pair.encoding_name, rest,
                  d.size(), res.hash_size);
}
  assert(res.hash_size);
  memcpy(res.hash, d.data(), res.hash_size);

  return res;
}

Hash Hash::parse_sri(std::string_view original, const experimental_feature_settings_t& xp_settings) {
  auto rest = original;

  // Parse the has type before the separator, if there was one.
  auto hash_raw = split_prefix_to(rest, '-');
  if (!hash_raw) {
    throw BadHash("hash '%s' is not SRI", original);
}
  hash_algorithm_t parsed_type = parse_hash_algo(*hash_raw, xp_settings);

  return parse_low_level(rest, parsed_type, {base64::decode, "SRI"}, xp_settings);
}

/**
 * @param rest is the string to parse
 *
 * @param resolve_algo resolves the parsed type (or throws an error when it is not
 * possible.)
 *
 * @return the parsed hash and the format it was parsed from
 */
static std::pair<Hash, hash_format_t> parse_any_helper(std::string_view rest, auto resolve_algo) {
  bool is_sri = false;

  // Parse the hash type before the separator, if there was one.
  std::optional<hash_algorithm_t> opt_parsed_algo;
  {
    auto hash_raw = split_prefix_to(rest, ':');

    if (!hash_raw) {
      hash_raw = split_prefix_to(rest, '-');
      if (hash_raw) {
        is_sri = true;
}
    }
    if (hash_raw) {
      opt_parsed_algo = parse_hash_algo(*hash_raw);
}
  }

  hash_algorithm_t algo = resolve_algo(std::move(opt_parsed_algo));

  auto [decode, formatName,
        format] = [&]() -> std::tuple<decltype(base16::decode)*, std::string_view, hash_format_t> {
    if (is_sri) {
      /* In the SRI case, we always are using base64. If the
         length is wrong, get an error later. */
      return {base64::decode, "SRI", hash_format_t::SRI};
    } else {
      /* Otherwise, decide via the length of the hash (for the
         given algorithm) what base encoding it is. */
      auto format = base_from_size(rest, algo);
      auto [decode, formatName] = base_explicit(format);
      return {decode, formatName, format};
    }
  }();

  return {parse_low_level(rest, algo, {decode, formatName}), format};
}

Hash Hash::parse_any_prefixed(std::string_view original) {
  return parse_any_helper(original,
                        [&](std::optional<hash_algorithm_t> opt_parsed_algo) {
                          // Either the string or user must provide the type, if they both do they
                          // must agree.
                          if (!opt_parsed_algo) {
                            throw BadHash("hash '%s' does not include a type", original);
}

                          return *opt_parsed_algo;
                        })
      .first;
}

Hash Hash::parse_any(std::string_view original, std::optional<hash_algorithm_t> opt_algo) {
  return parse_any_returning_format(original, opt_algo).first;
}

std::pair<Hash, hash_format_t> Hash::parse_any_returning_format(std::string_view original,
                                                          std::optional<hash_algorithm_t> opt_algo) {
  return parse_any_helper(original, [&](std::optional<hash_algorithm_t> opt_parsed_algo) {
    // Either the string or user must provide the type, if they both do they
    // must agree.
    if (!opt_parsed_algo && !opt_algo) {
      throw BadHash(
          "hash '%s' does not include a type, nor is the type otherwise known from context",
          original);
    } else if (opt_parsed_algo && opt_algo && *opt_parsed_algo != *opt_algo) {
      throw BadHash("hash '%s' should have type '%s'", original, print_hash_algo(*opt_algo));
}

    return opt_parsed_algo ? *opt_parsed_algo : *opt_algo;
  });
}

Hash Hash::parse_non_sri_unprefixed(std::string_view s, hash_algorithm_t algo) {
  return parse_explicit_format_unprefixed(s, algo, base_from_size(s, algo));
}

Hash Hash::parse_explicit_format_unprefixed(std::string_view s, hash_algorithm_t algo, hash_format_t format,
                                         const experimental_feature_settings_t& xp_settings) {
  return parse_low_level(s, algo, base_explicit(format), xp_settings);
}

Hash Hash::random(hash_algorithm_t algo) {
  Hash hash(algo);
  randombytes_buf(hash.hash, hash.hash_size);
  return hash;
}

Hash new_hash_allow_empty(std::string_view hash_str, std::optional<hash_algorithm_t> ha) {
  if (hash_str.empty()) {
    if (!ha) {
      throw BadHash("empty hash requires explicit hash algorithm");
}
    Hash h(*ha);
    warn("found empty hash, assuming '%s'", h.to_string(hash_format_t::SRI, true));
    return h;
  } else {
    return Hash::parse_any(hash_str, ha);
}
}

union Hash::Ctx {
  blake3_hasher blake3;
  MD5_CTX md5;
  SHA_CTX sha1;
  SHA256_CTX sha256;
  SHA512_CTX sha512;
};

static void start(hash_algorithm_t ha, Hash::Ctx& ctx) {
  if (ha == hash_algorithm_t::BLAKE3) {
    blake3_hasher_init(&ctx.blake3);
  } else if (ha == hash_algorithm_t::MD5) {
    MD5_Init(&ctx.md5);
  } else if (ha == hash_algorithm_t::SHA1) {
    SHA1_Init(&ctx.sha1);
  } else if (ha == hash_algorithm_t::SHA256) {
    SHA256_Init(&ctx.sha256);
  } else if (ha == hash_algorithm_t::SHA512) {
    SHA512_Init(&ctx.sha512);
}
}

// BLAKE3 data size threshold beyond which parallel hashing with TBB is likely faster.
//
// NOTE: This threshold is based on the recommended rule-of-thumb from the official BLAKE3
// documentation for typical x86_64 hardware as of 2025. In the future it may make sense to allow
// the user to tune this through nix.conf.
const size_t blake3_tbb_threshold = 128000;

// Decide which BLAKE3 update strategy to use based on some heuristics. Currently this just checks
// the data size but in the future it might also take into consideration available system resources
// or the presence of a shared-memory capable GPU for a heterogenous compute implementation.
void blake3_hasher_update_with_heuristics(blake3_hasher* blake3, std::string_view data) {
#ifdef BLAKE3_USE_TBB
  if (data.size() >= blake3_tbb_threshold) {
    blake3_hasher_update_tbb(blake3, data.data(), data.size());
  } else
#endif
  {
    blake3_hasher_update(blake3, data.data(), data.size());
  }
}

static void update(hash_algorithm_t ha, Hash::Ctx& ctx, std::string_view data) {
  if (ha == hash_algorithm_t::BLAKE3) {
    blake3_hasher_update_with_heuristics(&ctx.blake3, data);
  } else if (ha == hash_algorithm_t::MD5) {
    MD5_Update(&ctx.md5, data.data(), data.size());
  } else if (ha == hash_algorithm_t::SHA1) {
    SHA1_Update(&ctx.sha1, data.data(), data.size());
  } else if (ha == hash_algorithm_t::SHA256) {
    SHA256_Update(&ctx.sha256, data.data(), data.size());
  } else if (ha == hash_algorithm_t::SHA512) {
    SHA512_Update(&ctx.sha512, data.data(), data.size());
}
}

static void finish(hash_algorithm_t ha, Hash::Ctx& ctx, unsigned char* hash) {
  if (ha == hash_algorithm_t::BLAKE3) {
    blake3_hasher_finalize(&ctx.blake3, hash, BLAKE3_OUT_LEN);
  } else if (ha == hash_algorithm_t::MD5) {
    MD5_Final(hash, &ctx.md5);
  } else if (ha == hash_algorithm_t::SHA1) {
    SHA1_Final(hash, &ctx.sha1);
  } else if (ha == hash_algorithm_t::SHA256) {
    SHA256_Final(hash, &ctx.sha256);
  } else if (ha == hash_algorithm_t::SHA512) {
    SHA512_Final(hash, &ctx.sha512);
}
}

Hash hash_string(hash_algorithm_t ha, std::string_view s,
                const experimental_feature_settings_t& xp_settings) {
  Hash::Ctx ctx;
  Hash hash(ha, xp_settings);
  start(ha, ctx);
  update(ha, ctx, s);
  finish(ha, ctx, hash.hash);
  return hash;
}

Hash hash_file(hash_algorithm_t ha, const Path& path) {
  hash_sink_t sink(ha);
  read_file(path, sink);
  return sink.finish().hash;
}

hash_sink_t::hash_sink_t(hash_algorithm_t ha) : ha(ha) {
  ctx = new Hash::Ctx;
  bytes = 0;
  start(ha, *ctx);
}

hash_sink_t::~hash_sink_t() {
  buf_pos = 0;
  delete ctx;
}

void hash_sink_t::write_unbuffered(std::string_view data) {
  bytes += data.size();
  update(ha, *ctx, data);
}

hash_result_t hash_sink_t::finish() {
  flush();
  Hash hash(ha);
  nix::finish(ha, *ctx, hash.hash);
  return hash_result_t(hash, bytes);
}

hash_result_t hash_sink_t::current_hash() {
  flush();
  Hash::Ctx ctx2 = *ctx;
  Hash hash(ha);
  nix::finish(ha, ctx2, hash.hash);
  return hash_result_t(hash, bytes);
}

Hash compress_hash(const Hash& hash, unsigned int new_size) {
  Hash h(hash.algo);
  h.hash_size = new_size;
  for (unsigned int i = 0; i < hash.hash_size; ++i) {
    h.hash[i % new_size] ^= hash.hash[i];
}
  return h;
}

std::optional<hash_format_t> parse_hash_format_opt(std::string_view hash_format_name) {
  if (hash_format_name == "base16") {
    return hash_format_t::base16;
}
  if (hash_format_name == "nix32") {
    return hash_format_t::nix32;
}
  if (hash_format_name == "base32") {
    warn(R"("base32" is a deprecated alias for hash format "nix32".)");
    return hash_format_t::nix32;
  }
  if (hash_format_name == "base64") {
    return hash_format_t::base64;
}
  if (hash_format_name == "sri") {
    return hash_format_t::SRI;
}
  return std::nullopt;
}

hash_format_t parse_hash_format(std::string_view hash_format_name) {
  auto opt_f = parse_hash_format_opt(hash_format_name);
  if (opt_f) {
    return *opt_f;
}
  throw UsageError("unknown hash format '%1%', expect 'base16', 'base32', 'base64', or 'sri'",
                   hash_format_name);
}

std::string_view print_hash_format(hash_format_t hash_format_t) {
  switch (hash_format_t) {
    case hash_format_t::base64:
      return "base64";
    case hash_format_t::nix32:
      return "nix32";
    case hash_format_t::base16:
      return "base16";
    case hash_format_t::SRI:
      return "sri";
    default:
      // illegal hash base enum value internally, as opposed to external input
      // which should be validated with nice error message.
      assert(false);
  }
}

std::optional<hash_algorithm_t> parse_hash_algo_opt(std::string_view s,
                                              const experimental_feature_settings_t& xp_settings) {
  if (s == "blake3") {
    xp_settings.require(xp_t::blak_e3_hashes);
    return hash_algorithm_t::BLAKE3;
  }
  if (s == "md5") {
    return hash_algorithm_t::MD5;
}
  if (s == "sha1") {
    return hash_algorithm_t::SHA1;
}
  if (s == "sha256") {
    return hash_algorithm_t::SHA256;
}
  if (s == "sha512") {
    return hash_algorithm_t::SHA512;
}
  return std::nullopt;
}

hash_algorithm_t parse_hash_algo(std::string_view s, const experimental_feature_settings_t& xp_settings) {
  auto opt_h = parse_hash_algo_opt(s, xp_settings);
  if (opt_h) {
    return *opt_h;
  } else {
    throw UsageError(
        "unknown hash algorithm '%1%', expect 'blake3', 'md5', 'sha1', 'sha256', or 'sha512'", s);
}
}

std::string_view print_hash_algo(hash_algorithm_t ha) {
  switch (ha) {
    case hash_algorithm_t::BLAKE3:
      return "blake3";
    case hash_algorithm_t::MD5:
      return "md5";
    case hash_algorithm_t::SHA1:
      return "sha1";
    case hash_algorithm_t::SHA256:
      return "sha256";
    case hash_algorithm_t::SHA512:
      return "sha512";
    default:
      // illegal hash type enum value internally, as opposed to external input
      // which should be validated with nice error message.
      assert(false);
  }
}

} // namespace nix

namespace nlohmann {

using namespace nix;

Hash adl_serializer<Hash>::from_json(const json& json,
                                     const experimental_feature_settings_t& xp_settings) {
  auto& s = get_string(json);
  return Hash::parse_sri(s, xp_settings);
}

void adl_serializer<Hash>::to_json(json& json, const Hash& hash) {
  json = hash.to_string(hash_format_t::SRI, true);
}

} // namespace nlohmann
