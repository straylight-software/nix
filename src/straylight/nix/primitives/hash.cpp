// straylight::nix::primitives::hash
//
// Implementation of high-performance hash functions.
// Uses BLAKE3 official C library and OpenSSL for SHA family.

#include "hash.h"

#include <algorithm>
#include <compare>
#include <cstring>
#include <stdexcept>

// BLAKE3 official C library
#include <blake3.h>

// OpenSSL for SHA family (uses SHA-NI when available)
#include <openssl/evp.h>
#include <openssl/md5.h>
#include <openssl/sha.h>

namespace straylight::nix::primitives::hash {

// ─────────────────────────────────────────────────────────────────────────────
// Base16 (hex) encoding/decoding
// ─────────────────────────────────────────────────────────────────────────────

namespace {

constexpr char hex_chars[] = "0123456789abcdef";

std::string to_hex_impl(std::span<const uint8_t> data) {
  std::string result;
  result.reserve(data.size() * 2);
  for (uint8_t byte : data) {
    result += hex_chars[(byte >> 4) & 0xF];
    result += hex_chars[byte & 0xF];
  }
  return result;
}

uint8_t hex_digit(char c) {
  if (c >= '0' && c <= '9') {
    return static_cast<uint8_t>(c - '0');
  }
  if (c >= 'a' && c <= 'f') {
    return static_cast<uint8_t>(c - 'a' + 10);
  }
  if (c >= 'A' && c <= 'F') {
    return static_cast<uint8_t>(c - 'A' + 10);
  }
  throw std::invalid_argument("Invalid hex character");
}

void from_hex_impl(std::string_view hex, std::span<uint8_t> out) {
  if (hex.size() != out.size() * 2) {
    throw std::invalid_argument("Invalid hex string length");
  }
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = static_cast<uint8_t>((hex_digit(hex[i * 2]) << 4) | hex_digit(hex[i * 2 + 1]));
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Base64 encoding/decoding (RFC 4648)
// ─────────────────────────────────────────────────────────────────────────────

constexpr char base64_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string to_base64_impl(std::span<const uint8_t> data) {
  std::string result;
  result.reserve((data.size() + 2) / 3 * 4);

  std::size_t i = 0;
  while (i + 2 < data.size()) {
    uint32_t triple = (static_cast<uint32_t>(data[i]) << 16) |
                      (static_cast<uint32_t>(data[i + 1]) << 8) |
                      static_cast<uint32_t>(data[i + 2]);
    result += base64_chars[(triple >> 18) & 0x3F];
    result += base64_chars[(triple >> 12) & 0x3F];
    result += base64_chars[(triple >> 6) & 0x3F];
    result += base64_chars[triple & 0x3F];
    i += 3;
  }

  if (i + 1 == data.size()) {
    uint32_t val = static_cast<uint32_t>(data[i]) << 16;
    result += base64_chars[(val >> 18) & 0x3F];
    result += base64_chars[(val >> 12) & 0x3F];
    result += "==";
  } else if (i + 2 == data.size()) {
    uint32_t val =
        (static_cast<uint32_t>(data[i]) << 16) | (static_cast<uint32_t>(data[i + 1]) << 8);
    result += base64_chars[(val >> 18) & 0x3F];
    result += base64_chars[(val >> 12) & 0x3F];
    result += base64_chars[(val >> 6) & 0x3F];
    result += '=';
  }

  return result;
}

int8_t base64_decode_char(char c) {
  if (c >= 'A' && c <= 'Z') {
    return static_cast<int8_t>(c - 'A');
  }
  if (c >= 'a' && c <= 'z') {
    return static_cast<int8_t>(c - 'a' + 26);
  }
  if (c >= '0' && c <= '9') {
    return static_cast<int8_t>(c - '0' + 52);
  }
  if (c == '+') {
    return 62;
  }
  if (c == '/') {
    return 63;
  }
  if (c == '=') {
    return -1; // padding
  }
  throw std::invalid_argument("Invalid base64 character");
}

void from_base64_impl(std::string_view b64, std::span<uint8_t> out) {
  // Remove padding for length calculation
  std::size_t padding = 0;
  if (!b64.empty() && b64.back() == '=') {
    ++padding;
  }
  if (b64.size() > 1 && b64[b64.size() - 2] == '=') {
    ++padding;
  }

  std::size_t expected_size = (b64.size() / 4) * 3 - padding;
  if (out.size() != expected_size) {
    throw std::invalid_argument("Invalid base64 output size");
  }

  std::size_t out_idx = 0;
  for (std::size_t i = 0; i + 3 < b64.size(); i += 4) {
    int8_t a = base64_decode_char(b64[i]);
    int8_t b = base64_decode_char(b64[i + 1]);
    int8_t c = base64_decode_char(b64[i + 2]);
    int8_t d = base64_decode_char(b64[i + 3]);

    if (out_idx < out.size()) {
      out[out_idx++] = static_cast<uint8_t>((a << 2) | (b >> 4));
    }
    if (c >= 0 && out_idx < out.size()) {
      out[out_idx++] = static_cast<uint8_t>(((b & 0xF) << 4) | (c >> 2));
    }
    if (d >= 0 && out_idx < out.size()) {
      out[out_idx++] = static_cast<uint8_t>(((c & 0x3) << 6) | d);
    }
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Nix base32 encoding/decoding
//
// Nix uses a custom base32 alphabet that omits e, o, u, t to avoid
// potentially offensive words. The encoding is also reversed (LSB first).
// ─────────────────────────────────────────────────────────────────────────────

constexpr char nix32_chars[] = "0123456789abcdfghijklmnpqrsvwxyz";

std::string to_nix32_impl(std::span<const uint8_t> data) {
  // Output length: ceil(bits / 5)
  std::size_t bits = data.size() * 8;
  std::size_t len = (bits + 4) / 5;
  std::string result(len, '0');

  // Nix32 encodes LSB first (reversed)
  for (std::size_t i = 0; i < len; ++i) {
    std::size_t bit_pos = i * 5;
    std::size_t byte_pos = bit_pos / 8;
    std::size_t bit_offset = bit_pos % 8;

    uint8_t val = 0;
    if (byte_pos < data.size()) {
      val = data[byte_pos] >> bit_offset;
    }
    if (bit_offset > 3 && byte_pos + 1 < data.size()) {
      val |= data[byte_pos + 1] << (8 - bit_offset);
    }
    result[len - 1 - i] = nix32_chars[val & 0x1F];
  }

  return result;
}

int8_t nix32_decode_char(char c) {
  if (c >= '0' && c <= '9') {
    return static_cast<int8_t>(c - '0');
  }
  if (c >= 'a' && c <= 'd') {
    return static_cast<int8_t>(c - 'a' + 10);
  }
  if (c == 'f') {
    return 14;
  }
  if (c >= 'g' && c <= 'n') {
    return static_cast<int8_t>(c - 'g' + 15);
  }
  if (c == 'p') {
    return 23;
  }
  if (c == 'q') {
    return 24;
  }
  if (c == 'r') {
    return 25;
  }
  if (c == 's') {
    return 26;
  }
  if (c == 'v') {
    return 27;
  }
  if (c >= 'w' && c <= 'z') {
    return static_cast<int8_t>(c - 'w' + 28);
  }
  throw std::invalid_argument("Invalid nix32 character");
}

void from_nix32_impl(std::string_view nix32, std::span<uint8_t> out) {
  std::fill(out.begin(), out.end(), 0);

  for (std::size_t i = 0; i < nix32.size(); ++i) {
    uint8_t val = static_cast<uint8_t>(nix32_decode_char(nix32[nix32.size() - 1 - i]));
    std::size_t bit_pos = i * 5;
    std::size_t byte_pos = bit_pos / 8;
    std::size_t bit_offset = bit_pos % 8;

    if (byte_pos < out.size()) {
      out[byte_pos] |= val << bit_offset;
    }
    if (bit_offset > 3 && byte_pos + 1 < out.size()) {
      out[byte_pos + 1] |= val >> (8 - bit_offset);
    }
  }
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Hash class implementation
// ─────────────────────────────────────────────────────────────────────────────

Hash::Hash(Algorithm algo, std::span<const uint8_t> data) noexcept
    : size_(hash_size(algo)), algo_(algo) {
  auto copy_size = std::min(data.size(), size_);
  std::memcpy(bytes_.data(), data.data(), copy_size);
}

std::string Hash::to_hex() const {
  return to_hex_impl(bytes());
}

std::string Hash::to_base64() const {
  return to_base64_impl(bytes());
}

std::string Hash::to_nix32() const {
  return to_nix32_impl(bytes());
}

std::string Hash::to_sri() const {
  return std::string(algorithm_name(algo_)) + "-" + to_base64();
}

Hash Hash::from_hex(Algorithm algo, std::string_view hex) {
  Hash h(algo);
  from_hex_impl(hex, std::span<uint8_t>(h.data(), h.size()));
  return h;
}

Hash Hash::from_base64(Algorithm algo, std::string_view b64) {
  Hash h(algo);
  from_base64_impl(b64, std::span<uint8_t>(h.data(), h.size()));
  return h;
}

Hash Hash::from_nix32(Algorithm algo, std::string_view nix32) {
  Hash h(algo);
  from_nix32_impl(nix32, std::span<uint8_t>(h.data(), h.size()));
  return h;
}

Hash Hash::from_sri(std::string_view sri) {
  auto dash = sri.find('-');
  if (dash == std::string_view::npos) {
    throw std::invalid_argument("Invalid SRI format: missing dash");
  }

  auto algo_name = sri.substr(0, dash);
  auto b64 = sri.substr(dash + 1);

  Algorithm algo;
  if (algo_name == "md5") {
    algo = Algorithm::MD5;
  } else if (algo_name == "sha1") {
    algo = Algorithm::SHA1;
  } else if (algo_name == "sha256") {
    algo = Algorithm::SHA256;
  } else if (algo_name == "sha512") {
    algo = Algorithm::SHA512;
  } else if (algo_name == "blake3") {
    algo = Algorithm::BLAKE3;
  } else {
    throw std::invalid_argument("Unknown algorithm in SRI: " + std::string(algo_name));
  }

  return from_base64(algo, b64);
}

bool Hash::operator==(const Hash& other) const noexcept {
  if (algo_ != other.algo_ || size_ != other.size_) {
    return false;
  }
  return std::memcmp(bytes_.data(), other.bytes_.data(), size_) == 0;
}

std::strong_ordering Hash::operator<=>(const Hash& other) const noexcept {
  if (auto cmp = algo_ <=> other.algo_; cmp != 0) {
    return cmp;
  }
  if (auto cmp = size_ <=> other.size_; cmp != 0) {
    return cmp;
  }
  return std::memcmp(bytes_.data(), other.bytes_.data(), size_) <=> 0;
}

Hash Hash::compress(std::size_t new_size) const {
  if (new_size > size_) {
    throw std::invalid_argument("Cannot expand hash");
  }

  Hash result;
  result.size_ = new_size;
  result.algo_ = algo_;

  // XOR-fold the hash to compress it
  for (std::size_t i = 0; i < size_; ++i) {
    result.bytes_[i % new_size] ^= bytes_[i];
  }

  return result;
}

bool Hash::is_zero() const noexcept {
  for (std::size_t i = 0; i < size_; ++i) {
    if (bytes_[i] != 0) {
      return false;
    }
  }
  return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Hasher implementation
// ─────────────────────────────────────────────────────────────────────────────

// Context union to hold different hash states
union HasherContext {
  blake3_hasher blake3;
  MD5_CTX md5;
  SHA_CTX sha1;
  SHA256_CTX sha256;
  SHA512_CTX sha512;
};

Hasher::Hasher(Algorithm algo) : algo_(algo) {
  init();
}

Hasher::~Hasher() {
  cleanup();
}

Hasher::Hasher(Hasher&& other) noexcept
    : algo_(other.algo_), ctx_(other.ctx_), bytes_hashed_(other.bytes_hashed_) {
  other.ctx_ = nullptr;
}

Hasher& Hasher::operator=(Hasher&& other) noexcept {
  if (this != &other) {
    cleanup();
    algo_ = other.algo_;
    ctx_ = other.ctx_;
    bytes_hashed_ = other.bytes_hashed_;
    other.ctx_ = nullptr;
  }
  return *this;
}

void Hasher::init() {
  ctx_ = new HasherContext();
  auto* ctx = static_cast<HasherContext*>(ctx_);

  switch (algo_) {
    case Algorithm::BLAKE3:
      blake3_hasher_init(&ctx->blake3);
      break;
    case Algorithm::MD5:
      MD5_Init(&ctx->md5);
      break;
    case Algorithm::SHA1:
      SHA1_Init(&ctx->sha1);
      break;
    case Algorithm::SHA256:
      SHA256_Init(&ctx->sha256);
      break;
    case Algorithm::SHA512:
      SHA512_Init(&ctx->sha512);
      break;
  }
}

void Hasher::cleanup() {
  if (ctx_) {
    delete static_cast<HasherContext*>(ctx_);
    ctx_ = nullptr;
  }
}

void Hasher::update(std::string_view data) {
  update(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(data.data()), data.size()));
}

void Hasher::update(std::span<const uint8_t> data) {
  if (!ctx_) {
    throw std::runtime_error("Hasher already finalized");
  }

  auto* ctx = static_cast<HasherContext*>(ctx_);
  bytes_hashed_ += data.size();

  switch (algo_) {
    case Algorithm::BLAKE3:
      blake3_hasher_update(&ctx->blake3, data.data(), data.size());
      break;
    case Algorithm::MD5:
      MD5_Update(&ctx->md5, data.data(), data.size());
      break;
    case Algorithm::SHA1:
      SHA1_Update(&ctx->sha1, data.data(), data.size());
      break;
    case Algorithm::SHA256:
      SHA256_Update(&ctx->sha256, data.data(), data.size());
      break;
    case Algorithm::SHA512:
      SHA512_Update(&ctx->sha512, data.data(), data.size());
      break;
  }
}

Hash Hasher::finish() {
  if (!ctx_) {
    throw std::runtime_error("Hasher already finalized");
  }

  Hash result(algo_);
  auto* ctx = static_cast<HasherContext*>(ctx_);

  switch (algo_) {
    case Algorithm::BLAKE3:
      blake3_hasher_finalize(&ctx->blake3, result.data(), result.size());
      break;
    case Algorithm::MD5:
      MD5_Final(result.data(), &ctx->md5);
      break;
    case Algorithm::SHA1:
      SHA1_Final(result.data(), &ctx->sha1);
      break;
    case Algorithm::SHA256:
      SHA256_Final(result.data(), &ctx->sha256);
      break;
    case Algorithm::SHA512:
      SHA512_Final(result.data(), &ctx->sha512);
      break;
  }

  cleanup();
  return result;
}

Hash Hasher::current() const {
  if (!ctx_) {
    throw std::runtime_error("Hasher already finalized");
  }

  // Create a copy of the context to finalize without consuming
  HasherContext ctx_copy;
  std::memcpy(&ctx_copy, ctx_, sizeof(HasherContext));

  Hash result(algo_);

  switch (algo_) {
    case Algorithm::BLAKE3:
      blake3_hasher_finalize(&ctx_copy.blake3, result.data(), result.size());
      break;
    case Algorithm::MD5:
      MD5_Final(result.data(), &ctx_copy.md5);
      break;
    case Algorithm::SHA1:
      SHA1_Final(result.data(), &ctx_copy.sha1);
      break;
    case Algorithm::SHA256:
      SHA256_Final(result.data(), &ctx_copy.sha256);
      break;
    case Algorithm::SHA512:
      SHA512_Final(result.data(), &ctx_copy.sha512);
      break;
  }

  return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// One-shot hashing functions
// ─────────────────────────────────────────────────────────────────────────────

Hash compute(Algorithm algo, std::string_view data) {
  return compute(
      algo, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(data.data()), data.size()));
}

Hash compute(Algorithm algo, std::span<const uint8_t> data) {
  Hash result(algo);

  switch (algo) {
    case Algorithm::BLAKE3: {
      blake3_hasher hasher;
      blake3_hasher_init(&hasher);
      blake3_hasher_update(&hasher, data.data(), data.size());
      blake3_hasher_finalize(&hasher, result.data(), result.size());
      break;
    }
    case Algorithm::MD5:
      MD5(data.data(), data.size(), result.data());
      break;
    case Algorithm::SHA1:
      SHA1(data.data(), data.size(), result.data());
      break;
    case Algorithm::SHA256:
      SHA256(data.data(), data.size(), result.data());
      break;
    case Algorithm::SHA512:
      SHA512(data.data(), data.size(), result.data());
      break;
  }

  return result;
}

} // namespace straylight::nix::primitives::hash
