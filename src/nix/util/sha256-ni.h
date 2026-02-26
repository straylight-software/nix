// sha256-ni.h
//
// SHA256 implementation using Intel SHA-NI instructions.
//
// This provides a drop-in replacement for OpenSSL/LibreSSL SHA256 functions
// with significantly better performance on CPUs supporting SHA-NI (most x86_64
// since ~2016 Intel/AMD).
//
// Usage:
//   #include "sha256-ni.h"
//
//   sha256_ni_ctx ctx;
//   sha256_ni_init(&ctx);
//   sha256_ni_update(&ctx, data, len);
//   sha256_ni_final(hash, &ctx);
//
// Runtime detection:
//   if (sha256_ni_available()) {
//     // use SHA-NI implementation
//   } else {
//     // fallback to generic implementation
//   }

#pragma once

#include <cstddef>
#include <cstdint>

namespace nix {

// SHA256 context structure (matches OpenSSL SHA256_CTX layout for compatibility)
struct sha256_ni_ctx {
  uint32_t state[8];  // Hash state
  uint64_t count;     // Number of bytes processed
  uint8_t buffer[64]; // Input buffer for partial blocks
};

// Check if SHA-NI instructions are available at runtime
bool sha256_ni_available();

// Initialize SHA256 context
void sha256_ni_init(sha256_ni_ctx* ctx);

// Update hash with data (can be called multiple times)
void sha256_ni_update(sha256_ni_ctx* ctx, const void* data, size_t len);

// Finalize hash and write 32-byte result to hash
void sha256_ni_final(uint8_t hash[32], sha256_ni_ctx* ctx);

// One-shot hash (convenience function)
void sha256_ni(const void* data, size_t len, uint8_t hash[32]);

} // namespace nix
