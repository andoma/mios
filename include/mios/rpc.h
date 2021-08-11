#pragma once

#include "mios.h"
#include "error.h"

#include <stdint.h>
#include <stddef.h>

typedef struct rpc_method {
  const char *name;
  error_t (*invoke)(const void *in, void *out, size_t in_size);
  uint16_t in_size;
  uint16_t out_size;
  uint16_t flags;
  uint16_t stacksize;
} rpc_method_t;

#define RPC_MAY_BLOCK 0x1

#define RPC_DEF(name, in_size, out_size, fn, flags)                     \
  static rpc_method_t MIOS_JOIN(rpc, __LINE__) __attribute__ ((used, section("rpcdef"))) = { name, (void *)fn, in_size, out_size, flags};
