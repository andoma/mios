#pragma once

#include <stdint.h>

#define ntohs(x) __builtin_bswap16(x)
#define htons(x) __builtin_bswap16(x)

#define ntohl(x) __builtin_bswap32(x)
#define htonl(x) __builtin_bswap32(x)

uint32_t inet_addr(const char *s);

static inline uint32_t
mask_from_prefixlen(int prefixlen)
{
  if(prefixlen == 0)
    return 0; // 1 << 32 is undefined
  return ~((1 << (32 - prefixlen)) - 1);
}
