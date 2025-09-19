// This is a public domain implementation of SHA-512.
// Based on the SHA-256 implementation at https://github.com/983/SHA-256.

#include "sha512.h"

#define SHA512_BYTES_SIZE 64

static inline uint64_t rotr(uint64_t x, int n) {
  return (x >> n) | (x << (64 - n));
}

static inline uint64_t step1(uint64_t e, uint64_t f, uint64_t g) {
  return (rotr(e, 14) ^ rotr(e, 18) ^ rotr(e, 41)) + ((e & f) ^ ((~ e) & g));
}

static inline uint64_t step2(uint64_t a, uint64_t b, uint64_t c) {
  return (rotr(a, 28) ^ rotr(a, 34) ^ rotr(a, 39)) + ((a & b) ^ (a & c) ^ (b & c));
}

static inline void update_w(uint64_t *w, int i, const uint8_t *buffer) {
  int j;
  for(j = 0;j < 16;j++) {
    if (i < 16) {
      w[j] =
	((uint64_t)buffer[0] << (8*7)) |
	((uint64_t)buffer[1] << (8*6)) |
	((uint64_t)buffer[2] << (8*5)) |
	((uint64_t)buffer[3] << (8*4)) |
	((uint64_t)buffer[4] << (8*3)) |
	((uint64_t)buffer[5] << (8*2)) |
	((uint64_t)buffer[6] << (8*1)) |
	((uint64_t)buffer[7] << (8*0));
      buffer += 8;
    } else {
      uint64_t a = w[(j + 1) & 15];
      uint64_t b = w[(j + 14) & 15];
      uint64_t s0 = (rotr(a,  1) ^ rotr(a,  8) ^ (a >>  7));
      uint64_t s1 = (rotr(b, 19) ^ rotr(b, 61) ^ (b >>  6));
      w[j] += w[(j + 9) & 15] + s0 + s1;
    }
  }
}

static void sha512_block(SHA512_CTX *sha) {
  uint64_t *state = sha->state;

  static const uint64_t k[] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL, 0x3956c25bf348b538ULL,
    0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL, 0xd807aa98a3030242ULL, 0x12835b0145706fbeULL,
    0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL, 0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL,
    0xc19bf174cf692694ULL, 0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL, 0x983e5152ee66dfabULL,
    0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL, 0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL,
    0x06ca6351e003826fULL, 0x142929670a0e6e70ULL, 0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL,
    0x53380d139d95b3dfULL, 0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL, 0xd192e819d6ef5218ULL,
    0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL, 0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL,
    0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL, 0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL,
    0x682e6ff3d6b2b8a3ULL, 0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL, 0xca273eceea26619cULL,
    0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL, 0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL,
    0x113f9804bef90daeULL, 0x1b710b35131c471bULL, 0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL,
    0x431d67c49c100d4cULL, 0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL
  };

  uint64_t a = state[0];
  uint64_t b = state[1];
  uint64_t c = state[2];
  uint64_t d = state[3];
  uint64_t e = state[4];
  uint64_t f = state[5];
  uint64_t g = state[6];
  uint64_t h = state[7];

  uint64_t w[16];

  int i, j;
  for(i = 0;i < 80;i += 16) {
    update_w(w, i, sha->buffer);

    for(j = 0;j < 16;j += 4) {
      uint64_t temp;
      temp = h + step1(e, f, g) + k[i + j + 0] + w[j + 0];
      h = temp + d;
      d = temp + step2(a, b, c);
      temp = g + step1(h, e, f) + k[i + j + 1] + w[j + 1];
      g = temp + c;
      c = temp + step2(d, a, b);
      temp = f + step1(g, h, e) + k[i + j + 2] + w[j + 2];
      f = temp + b;
      b = temp + step2(c, d, a);
      temp = e + step1(f, g, h) + k[i + j + 3] + w[j + 3];
      e = temp + a;
      a = temp + step2(b, c, d);
    }
  }

  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
  state[4] += e;
  state[5] += f;
  state[6] += g;
  state[7] += h;
}


void
SHA512Init(SHA512_CTX *sha)
{
  sha->state[0] = 0x6a09e667f3bcc908ULL;
  sha->state[1] = 0xbb67ae8584caa73bULL;
  sha->state[2] = 0x3c6ef372fe94f82bULL;
  sha->state[3] = 0xa54ff53a5f1d36f1ULL;
  sha->state[4] = 0x510e527fade682d1ULL;
  sha->state[5] = 0x9b05688c2b3e6c1fULL;
  sha->state[6] = 0x1f83d9abfb41bd6bULL;
  sha->state[7] = 0x5be0cd19137e2179ULL;
  sha->n_bits = 0;
  sha->buffer_counter = 0;
}

static void
sha512_append_byte(SHA512_CTX  *sha, uint8_t byte) {
  sha->buffer[sha->buffer_counter++] = byte;
  sha->n_bits += 8;

  if (sha->buffer_counter == 128) {
    sha->buffer_counter = 0;
    sha512_block(sha);
  }
}


void
SHA512Update(SHA512_CTX *ctx, const void *data, size_t len)
{
  const uint8_t *u8 = data;
  for(size_t i = 0; i < len; i++) {
    sha512_append_byte(ctx, u8[i]);
  }
}

static void
sha512_finalize(SHA512_CTX *sha)
{
  int i;
  uint64_t n_bits = sha->n_bits;

  sha512_append_byte(sha, 0x80);

  while (sha->buffer_counter != 128 - 8) {
    sha512_append_byte(sha, 0);
  }

  for(i = 7;i >= 0;i--) {
    uint8_t byte = (n_bits >> 8 * i) & 0xff;
    sha512_append_byte(sha, byte);
  }
}


void
SHA512Final(uint8_t digest[static 64], SHA512_CTX *sha)
{
  int i, j;
  sha512_finalize(sha);
  uint8_t *ptr = digest;

  for(i = 0;i < 8;i++) {
    for(j = 7;j >= 0;j--) {
      *ptr++ = (sha->state[i] >> j * 8) & 0xff;
    }
  }
}

void
SHA512(uint8_t digest[static 64], const void *data, size_t len)
{
  SHA512_CTX ctx;
  SHA512Init(&ctx);
  SHA512Update(&ctx, data, len);
  SHA512Final(digest, &ctx);
}
