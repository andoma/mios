#pragma once

#include <stdint.h>

// AES-128, ECB, encryption only (enough for the BLE SMP crypto toolbox).

typedef struct {
  uint8_t round_keys[176]; // 11 round keys * 16 bytes
} aes128_t;

void aes128_init(aes128_t *ctx, const uint8_t key[16]);

// Encrypt one 16-byte block. in and out may alias.
void aes128_encrypt(const aes128_t *ctx, const uint8_t in[16], uint8_t out[16]);
