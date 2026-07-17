#pragma once

#include <stdint.h>
#include <stddef.h>

// AES-128-CMAC (RFC 4493). The MAC (16 bytes) authenticates msg of any length
// under key. Used by the BLE LE Secure Connections crypto toolbox.
void aes_cmac(const uint8_t key[16], const uint8_t *msg, size_t len,
              uint8_t mac[16]);
