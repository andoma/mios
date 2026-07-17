// BLE LE Secure Connections crypto toolbox. The f-functions are validated
// against the spec sample data; ECDH is micro-ecc P-256, validated against
// RFC 5903. Everything here is big-endian (spec-native); smp.c reverses at
// the wire boundary.

#include "smp_lesc.h"

#include "lib/crypto/aes.h"
#include "lib/crypto/aes_cmac.h"
#include "lib/crypto/micro-ecc/uECC.h"

#include "smp.h" // ble_rand

#include <string.h>

void
smp_f4(const uint8_t u[32], const uint8_t v[32], const uint8_t x[16],
       uint8_t z, uint8_t out[16])
{
  uint8_t m[65];
  memcpy(m, u, 32);
  memcpy(m + 32, v, 32);
  m[64] = z;
  aes_cmac(x, m, sizeof(m), out);
}

void
smp_f5(const uint8_t dhkey[32], const uint8_t n1[16], const uint8_t n2[16],
       const uint8_t a1[7], const uint8_t a2[7],
       uint8_t mackey[16], uint8_t ltk[16])
{
  static const uint8_t salt[16] = {
    0x6c, 0x88, 0x83, 0x91, 0xaa, 0xf5, 0xa5, 0x38,
    0x60, 0x37, 0x0b, 0xdb, 0x5a, 0x60, 0x83, 0xbe,
  };
  uint8_t t[16];
  aes_cmac(salt, dhkey, 32, t);

  // counter(1) || keyID "btle"(4) || N1(16) || N2(16) || A1(7) || A2(7) || length=256(2)
  uint8_t m[53];
  m[1] = 0x62; m[2] = 0x74; m[3] = 0x6c; m[4] = 0x65;
  memcpy(m + 5, n1, 16);
  memcpy(m + 21, n2, 16);
  memcpy(m + 37, a1, 7);
  memcpy(m + 44, a2, 7);
  m[51] = 0x01; m[52] = 0x00;

  m[0] = 0;
  aes_cmac(t, m, sizeof(m), mackey);
  m[0] = 1;
  aes_cmac(t, m, sizeof(m), ltk);
}

void
smp_f6(const uint8_t w[16], const uint8_t n1[16], const uint8_t n2[16],
       const uint8_t r[16], const uint8_t iocap[3],
       const uint8_t a1[7], const uint8_t a2[7], uint8_t out[16])
{
  uint8_t m[65];
  memcpy(m, n1, 16);
  memcpy(m + 16, n2, 16);
  memcpy(m + 32, r, 16);
  memcpy(m + 48, iocap, 3);
  memcpy(m + 51, a1, 7);
  memcpy(m + 58, a2, 7);
  aes_cmac(w, m, sizeof(m), out);
}

uint32_t
smp_g2(const uint8_t u[32], const uint8_t v[32],
       const uint8_t x[16], const uint8_t y[16])
{
  uint8_t m[80], mac[16];
  memcpy(m, u, 32);
  memcpy(m + 32, v, 32);
  memcpy(m + 64, y, 16);
  aes_cmac(x, m, sizeof(m), mac);
  return (mac[12] << 24) | (mac[13] << 16) | (mac[14] << 8) | mac[15];
}

void
smp_ah(const uint8_t irk[16], const uint8_t r[3], uint8_t out[3])
{
  // ah(k, r) = e(k, 0^104 || r) mod 2^24. r' has r in the least-significant
  // bytes; everything big-endian.
  uint8_t rp[16] = {0};
  rp[13] = r[0];
  rp[14] = r[1];
  rp[15] = r[2];
  aes128_t ctx;
  aes128_init(&ctx, irk);
  uint8_t e[16];
  aes128_encrypt(&ctx, rp, e);
  out[0] = e[13];
  out[1] = e[14];
  out[2] = e[15];
}


// --- P-256 ECDH (micro-ecc) -------------------------------------------------

static int
ecc_rng(uint8_t *dest, unsigned size)
{
  ble_rand(dest, size);
  return 1;
}

static int rng_registered;

static void
ensure_rng(void)
{
  if(!rng_registered) {
    uECC_set_rng(ecc_rng);
    rng_registered = 1;
  }
}

int
smp_ecdh_keygen(uint8_t pub[64], uint8_t priv[32])
{
  ensure_rng();
  return uECC_make_key(pub, priv, uECC_secp256r1());
}

int
smp_ecdh_shared(const uint8_t peer_pub[64], const uint8_t priv[32],
                uint8_t dhkey[32])
{
  return uECC_shared_secret(peer_pub, priv, dhkey, uECC_secp256r1());
}

int
smp_ecdh_valid(const uint8_t pub[64])
{
  return uECC_valid_public_key(pub, uECC_secp256r1());
}
