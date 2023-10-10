#include "base64.h"

static const char b64tbl[] =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int
base64_encode(char *out, int out_size, const uint8_t *in, int in_size)
{
  while(in_size >= 3) {
    *out++ = b64tbl[in[0] >> 2];
    *out++ = b64tbl[((in[0] << 4) | (in[1] >> 4)) & 0x3f];
    *out++ = b64tbl[((in[1] << 2) | (in[2] >> 6)) & 0x3f];
    *out++ = b64tbl[in[2] & 0x3f];
    in += 3;
    in_size -= 3;
  }

  switch(in_size) {
  case 0:
    break;

  case 2:
    *out++ = b64tbl[in[0] >> 2];
    *out++ = b64tbl[((in[0] << 4) | (in[1] >> 4)) & 0x3f];
    *out++ = b64tbl[(in[1] << 2) & 0x3f];
    *out++ = '=';
    break;

  case 1:
    *out++ = b64tbl[in[0] >> 2];
    *out++ = b64tbl[(in[0] << 4) & 0x3f];
    *out++ = '=';
    *out++ = '=';
    break;
  }
  *out = 0;
  return 0;
}
