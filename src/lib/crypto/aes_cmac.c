#include "aes_cmac.h"
#include "aes.h"

#include <string.h>

// AES-128-CMAC, RFC 4493.

// Left-shift a 128-bit big-endian block by one bit.
static void
shl1(uint8_t *out, const uint8_t *in)
{
  uint8_t carry = 0;
  for(int i = 15; i >= 0; i--) {
    uint8_t b = in[i];
    out[i] = (b << 1) | carry;
    carry = b >> 7;
  }
}

// Rb = 0x87 for the 128-bit block size.
static void
subkey(uint8_t *k, const uint8_t *l)
{
  const int msb = l[0] & 0x80;
  shl1(k, l);
  if(msb)
    k[15] ^= 0x87;
}

void
aes_cmac(const uint8_t key[16], const uint8_t *msg, size_t len,
         uint8_t mac[16])
{
  aes128_t ctx;
  aes128_init(&ctx, key);

  // Subkeys K1/K2 from L = AES(key, 0).
  uint8_t l[16] = {0};
  aes128_encrypt(&ctx, l, l);
  uint8_t k1[16], k2[16];
  subkey(k1, l);
  subkey(k2, k1);

  const size_t nblk = len ? (len + 15) / 16 : 1;
  const int complete = len && (len % 16) == 0;

  uint8_t x[16] = {0};
  uint8_t block[16];

  for(size_t i = 0; i < nblk; i++) {
    const size_t off = i * 16;
    if(i + 1 < nblk) {
      memcpy(block, msg + off, 16);
    } else if(complete) {
      // Last full block: XOR K1.
      for(int j = 0; j < 16; j++)
        block[j] = msg[off + j] ^ k1[j];
    } else {
      // Last partial (or empty) block: pad 10*, XOR K2.
      const size_t rem = len - off;
      for(size_t j = 0; j < 16; j++) {
        uint8_t b = j < rem ? msg[off + j] : (j == rem ? 0x80 : 0x00);
        block[j] = b ^ k2[j];
      }
    }
    for(int j = 0; j < 16; j++)
      x[j] ^= block[j];
    aes128_encrypt(&ctx, x, x);
  }

  memcpy(mac, x, 16);
}
