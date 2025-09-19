#pragma once

#include <stdint.h>
#include <stddef.h>

// This is a public domain implementation of SHA-512.
// Based on the SHA-256 implementation at https://github.com/983/SHA-256.

typedef struct {
  uint64_t state[8];
  uint8_t buffer[128];
  uint64_t n_bits;
  uint8_t buffer_counter;
} SHA512_CTX;

void SHA512Init(SHA512_CTX *ctx);

void SHA512Update(SHA512_CTX *ctx, const void *data, size_t len);

void SHA512Final(uint8_t digest[static 64], SHA512_CTX *ctx);

void SHA512(uint8_t digest[static 64], const void *src, size_t len);
