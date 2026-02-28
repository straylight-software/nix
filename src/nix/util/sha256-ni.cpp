// sha256-ni.cpp
//
// SHA256 implementation using Intel SHA-NI instructions.
//
// Based on Intel's SHA Extensions white paper and reference implementation.
// https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sha-extensions.html
//
// Performance: ~3-4x faster than generic C implementation on supported CPUs.

#include "sha256-ni.h"

#include <cstring>

#if defined(__x86_64__) || defined(_M_X64)
#  include <cpuid.h>
#  include <immintrin.h>
#endif

namespace nix {

// SHA256 initial hash values (first 32 bits of fractional parts of square roots of first 8 primes)
static const uint32_t sha256_initial_state[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                                 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

// SHA256 round constants (first 32 bits of fractional parts of cube roots of first 64 primes)
alignas(16) static const uint32_t sha256_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

// Byte swap constant for converting between big-endian and little-endian
alignas(16) static const uint8_t sha256_bswap_mask[16] = {3,  2,  1, 0, 7,  6,  5,  4,
                                                          11, 10, 9, 8, 15, 14, 13, 12};

#if defined(__x86_64__) || defined(_M_X64)

// Check for SHA-NI support using CPUID
bool sha256_ni_available() {
  static int cached = -1;
  if (cached >= 0) {
    return cached != 0;
  }

  unsigned int eax = 0;
  unsigned int ebx = 0;
  unsigned int ecx = 0;
  unsigned int edx = 0;

  // Check for SHA-NI (CPUID.7.0:EBX.SHA[bit 29])
  if (__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx)) {
    cached = (ebx & (1 << 29)) != 0 ? 1 : 0;
  } else {
    cached = 0;
  }

  return cached != 0;
}

// Process one 64-byte block using SHA-NI instructions
// Based on the reference implementation from Jeffrey Walton / Intel / Sean Gulley (miTLS)
// target attribute enables SHA-NI and SSSE3 instructions for this function only
__attribute__((target("sha,ssse3,sse4.1"))) static void
sha256_ni_process_block(uint32_t state[8], const uint8_t block[64]) {
  __m128i state0;
  __m128i state1;
  __m128i msg;
  __m128i tmp;
  __m128i msg0;
  __m128i msg1;
  __m128i msg2;
  __m128i msg3;
  __m128i abef_save;
  __m128i cdgh_save;

  const __m128i bswap_mask = _mm_load_si128(reinterpret_cast<const __m128i*>(sha256_bswap_mask));

  // Load initial state
  // state0 = [A, B, E, F]
  // state1 = [C, D, G, H]
  tmp = _mm_loadu_si128(reinterpret_cast<const __m128i*>(state));
  state1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(state + 4));

  tmp = _mm_shuffle_epi32(tmp, 0xB1);          // CDAB
  state1 = _mm_shuffle_epi32(state1, 0x1B);    // EFGH
  state0 = _mm_alignr_epi8(tmp, state1, 8);    // ABEF
  state1 = _mm_blend_epi16(state1, tmp, 0xF0); // CDGH

  // Save state for addition after rounds
  abef_save = state0;
  cdgh_save = state1;

  // Rounds 0-3
  msg0 = _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(block + 0)), bswap_mask);
  msg = _mm_add_epi32(msg0, _mm_load_si128(reinterpret_cast<const __m128i*>(sha256_k + 0)));
  state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
  msg = _mm_shuffle_epi32(msg, 0x0E);
  state0 = _mm_sha256rnds2_epu32(state0, state1, msg);

  // Rounds 4-7
  msg1 =
      _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(block + 16)), bswap_mask);
  msg = _mm_add_epi32(msg1, _mm_load_si128(reinterpret_cast<const __m128i*>(sha256_k + 4)));
  state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
  msg = _mm_shuffle_epi32(msg, 0x0E);
  state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
  msg0 = _mm_sha256msg1_epu32(msg0, msg1);

  // Rounds 8-11
  msg2 =
      _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(block + 32)), bswap_mask);
  msg = _mm_add_epi32(msg2, _mm_load_si128(reinterpret_cast<const __m128i*>(sha256_k + 8)));
  state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
  msg = _mm_shuffle_epi32(msg, 0x0E);
  state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
  msg1 = _mm_sha256msg1_epu32(msg1, msg2);

  // Rounds 12-15
  msg3 =
      _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(block + 48)), bswap_mask);
  msg = _mm_add_epi32(msg3, _mm_load_si128(reinterpret_cast<const __m128i*>(sha256_k + 12)));
  state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
  tmp = _mm_alignr_epi8(msg3, msg2, 4);
  msg0 = _mm_add_epi32(msg0, tmp);
  msg0 = _mm_sha256msg2_epu32(msg0, msg3);
  msg = _mm_shuffle_epi32(msg, 0x0E);
  state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
  msg2 = _mm_sha256msg1_epu32(msg2, msg3);

  // Rounds 16-19
  msg = _mm_add_epi32(msg0, _mm_load_si128(reinterpret_cast<const __m128i*>(sha256_k + 16)));
  state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
  tmp = _mm_alignr_epi8(msg0, msg3, 4);
  msg1 = _mm_add_epi32(msg1, tmp);
  msg1 = _mm_sha256msg2_epu32(msg1, msg0);
  msg = _mm_shuffle_epi32(msg, 0x0E);
  state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
  msg3 = _mm_sha256msg1_epu32(msg3, msg0);

  // Rounds 20-23
  msg = _mm_add_epi32(msg1, _mm_load_si128(reinterpret_cast<const __m128i*>(sha256_k + 20)));
  state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
  tmp = _mm_alignr_epi8(msg1, msg0, 4);
  msg2 = _mm_add_epi32(msg2, tmp);
  msg2 = _mm_sha256msg2_epu32(msg2, msg1);
  msg = _mm_shuffle_epi32(msg, 0x0E);
  state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
  msg0 = _mm_sha256msg1_epu32(msg0, msg1);

  // Rounds 24-27
  msg = _mm_add_epi32(msg2, _mm_load_si128(reinterpret_cast<const __m128i*>(sha256_k + 24)));
  state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
  tmp = _mm_alignr_epi8(msg2, msg1, 4);
  msg3 = _mm_add_epi32(msg3, tmp);
  msg3 = _mm_sha256msg2_epu32(msg3, msg2);
  msg = _mm_shuffle_epi32(msg, 0x0E);
  state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
  msg1 = _mm_sha256msg1_epu32(msg1, msg2);

  // Rounds 28-31
  msg = _mm_add_epi32(msg3, _mm_load_si128(reinterpret_cast<const __m128i*>(sha256_k + 28)));
  state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
  tmp = _mm_alignr_epi8(msg3, msg2, 4);
  msg0 = _mm_add_epi32(msg0, tmp);
  msg0 = _mm_sha256msg2_epu32(msg0, msg3);
  msg = _mm_shuffle_epi32(msg, 0x0E);
  state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
  msg2 = _mm_sha256msg1_epu32(msg2, msg3);

  // Rounds 32-35
  msg = _mm_add_epi32(msg0, _mm_load_si128(reinterpret_cast<const __m128i*>(sha256_k + 32)));
  state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
  tmp = _mm_alignr_epi8(msg0, msg3, 4);
  msg1 = _mm_add_epi32(msg1, tmp);
  msg1 = _mm_sha256msg2_epu32(msg1, msg0);
  msg = _mm_shuffle_epi32(msg, 0x0E);
  state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
  msg3 = _mm_sha256msg1_epu32(msg3, msg0);

  // Rounds 36-39
  msg = _mm_add_epi32(msg1, _mm_load_si128(reinterpret_cast<const __m128i*>(sha256_k + 36)));
  state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
  tmp = _mm_alignr_epi8(msg1, msg0, 4);
  msg2 = _mm_add_epi32(msg2, tmp);
  msg2 = _mm_sha256msg2_epu32(msg2, msg1);
  msg = _mm_shuffle_epi32(msg, 0x0E);
  state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
  msg0 = _mm_sha256msg1_epu32(msg0, msg1);

  // Rounds 40-43
  msg = _mm_add_epi32(msg2, _mm_load_si128(reinterpret_cast<const __m128i*>(sha256_k + 40)));
  state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
  tmp = _mm_alignr_epi8(msg2, msg1, 4);
  msg3 = _mm_add_epi32(msg3, tmp);
  msg3 = _mm_sha256msg2_epu32(msg3, msg2);
  msg = _mm_shuffle_epi32(msg, 0x0E);
  state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
  msg1 = _mm_sha256msg1_epu32(msg1, msg2);

  // Rounds 44-47
  msg = _mm_add_epi32(msg3, _mm_load_si128(reinterpret_cast<const __m128i*>(sha256_k + 44)));
  state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
  tmp = _mm_alignr_epi8(msg3, msg2, 4);
  msg0 = _mm_add_epi32(msg0, tmp);
  msg0 = _mm_sha256msg2_epu32(msg0, msg3);
  msg = _mm_shuffle_epi32(msg, 0x0E);
  state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
  msg2 = _mm_sha256msg1_epu32(msg2, msg3);

  // Rounds 48-51
  msg = _mm_add_epi32(msg0, _mm_load_si128(reinterpret_cast<const __m128i*>(sha256_k + 48)));
  state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
  tmp = _mm_alignr_epi8(msg0, msg3, 4);
  msg1 = _mm_add_epi32(msg1, tmp);
  msg1 = _mm_sha256msg2_epu32(msg1, msg0);
  msg = _mm_shuffle_epi32(msg, 0x0E);
  state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
  msg3 = _mm_sha256msg1_epu32(msg3, msg0);

  // Rounds 52-55
  msg = _mm_add_epi32(msg1, _mm_load_si128(reinterpret_cast<const __m128i*>(sha256_k + 52)));
  state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
  tmp = _mm_alignr_epi8(msg1, msg0, 4);
  msg2 = _mm_add_epi32(msg2, tmp);
  msg2 = _mm_sha256msg2_epu32(msg2, msg1);
  msg = _mm_shuffle_epi32(msg, 0x0E);
  state0 = _mm_sha256rnds2_epu32(state0, state1, msg);

  // Rounds 56-59
  msg = _mm_add_epi32(msg2, _mm_load_si128(reinterpret_cast<const __m128i*>(sha256_k + 56)));
  state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
  tmp = _mm_alignr_epi8(msg2, msg1, 4);
  msg3 = _mm_add_epi32(msg3, tmp);
  msg3 = _mm_sha256msg2_epu32(msg3, msg2);
  msg = _mm_shuffle_epi32(msg, 0x0E);
  state0 = _mm_sha256rnds2_epu32(state0, state1, msg);

  // Rounds 60-63
  msg = _mm_add_epi32(msg3, _mm_load_si128(reinterpret_cast<const __m128i*>(sha256_k + 60)));
  state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
  msg = _mm_shuffle_epi32(msg, 0x0E);
  state0 = _mm_sha256rnds2_epu32(state0, state1, msg);

  // Add saved state
  state0 = _mm_add_epi32(state0, abef_save);
  state1 = _mm_add_epi32(state1, cdgh_save);

  // Reorder state back to [A, B, C, D, E, F, G, H]
  tmp = _mm_shuffle_epi32(state0, 0x1B);       // FEBA
  state1 = _mm_shuffle_epi32(state1, 0xB1);    // DCHG
  state0 = _mm_blend_epi16(tmp, state1, 0xF0); // DCBA
  state1 = _mm_alignr_epi8(state1, tmp, 8);    // HGFE

  // Store result
  _mm_storeu_si128(reinterpret_cast<__m128i*>(state), state0);
  _mm_storeu_si128(reinterpret_cast<__m128i*>(state + 4), state1);
}

#else

// Non-x86_64: SHA-NI not available
bool sha256_ni_available() {
  return false;
}

static void sha256_ni_process_block(uint32_t state[8], const uint8_t block[64]) {
  // Should never be called on non-x86_64
  (void)state;
  (void)block;
}

#endif

void sha256_ni_init(sha256_ni_ctx* ctx) {
  std::memcpy(ctx->state, sha256_initial_state, sizeof(ctx->state));
  ctx->count = 0;
  std::memset(ctx->buffer, 0, sizeof(ctx->buffer));
}

void sha256_ni_update(sha256_ni_ctx* ctx, const void* data, size_t len) {
  const auto* input = static_cast<const uint8_t*>(data);
  size_t buffer_used = ctx->count % 64;

  ctx->count += len;

  // Fill partial buffer if we have leftover data
  if (buffer_used > 0) {
    size_t to_copy = 64 - buffer_used;
    if (to_copy > len) {
      to_copy = len;
    }
    std::memcpy(ctx->buffer + buffer_used, input, to_copy);
    input += to_copy;
    len -= to_copy;
    buffer_used += to_copy;

    if (buffer_used == 64) {
      sha256_ni_process_block(ctx->state, ctx->buffer);
      buffer_used = 0;
    }
  }

  // Process full blocks
  while (len >= 64) {
    sha256_ni_process_block(ctx->state, input);
    input += 64;
    len -= 64;
  }

  // Save remaining data
  if (len > 0) {
    std::memcpy(ctx->buffer, input, len);
  }
}

void sha256_ni_final(uint8_t hash[32], sha256_ni_ctx* ctx) {
  size_t buffer_used = ctx->count % 64;

  // Add padding (0x80 followed by zeros)
  ctx->buffer[buffer_used++] = 0x80;

  // If not enough room for length, process this block and start a new one
  if (buffer_used > 56) {
    std::memset(ctx->buffer + buffer_used, 0, 64 - buffer_used);
    sha256_ni_process_block(ctx->state, ctx->buffer);
    buffer_used = 0;
  }

  // Pad with zeros up to length field
  std::memset(ctx->buffer + buffer_used, 0, 56 - buffer_used);

  // Append length in bits as big-endian 64-bit integer
  uint64_t bit_count = ctx->count * 8;
  ctx->buffer[56] = static_cast<uint8_t>(bit_count >> 56);
  ctx->buffer[57] = static_cast<uint8_t>(bit_count >> 48);
  ctx->buffer[58] = static_cast<uint8_t>(bit_count >> 40);
  ctx->buffer[59] = static_cast<uint8_t>(bit_count >> 32);
  ctx->buffer[60] = static_cast<uint8_t>(bit_count >> 24);
  ctx->buffer[61] = static_cast<uint8_t>(bit_count >> 16);
  ctx->buffer[62] = static_cast<uint8_t>(bit_count >> 8);
  ctx->buffer[63] = static_cast<uint8_t>(bit_count);

  sha256_ni_process_block(ctx->state, ctx->buffer);

  // Convert state to big-endian output
  for (int i = 0; i < 8; i++) {
    hash[i * 4 + 0] = static_cast<uint8_t>(ctx->state[i] >> 24);
    hash[i * 4 + 1] = static_cast<uint8_t>(ctx->state[i] >> 16);
    hash[i * 4 + 2] = static_cast<uint8_t>(ctx->state[i] >> 8);
    hash[i * 4 + 3] = static_cast<uint8_t>(ctx->state[i]);
  }
}

void sha256_ni(const void* data, size_t len, uint8_t hash[32]) {
  sha256_ni_ctx ctx;
  sha256_ni_init(&ctx);
  sha256_ni_update(&ctx, data, len);
  sha256_ni_final(hash, &ctx);
}

} // namespace nix
