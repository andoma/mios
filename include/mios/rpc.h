#pragma once

#include "mios.h"
#include "error.h"

#include <stdint.h>
#include <stddef.h>

#define RPC_TYPE_INT     'i'
#define RPC_TYPE_FLOAT   'f'
#define RPC_TYPE_STRING  's'
#define RPC_TYPE_BINARY  'b'
#define RPC_TYPE_VOID    'v'

#define RPC_TYPE_CONST_STRING 'S'
#define RPC_TYPE_CONST_BINARY 'B'

typedef struct rpc_result {
  union {
    struct {
      void *data;
      size_t size;
    };
    int i32;
    float flt;
  };
  char type;
} rpc_result_t;

typedef struct rpc_method {
  const char *signature;
  void *invoke;
} rpc_method_t;

#define RPC_DEF(signature, fn)                     \
  static const rpc_method_t MIOS_JOIN(rpc, __LINE__) __attribute__ ((used, section("rpcdef"))) = { signature, fn };

error_t rpc_dispatch_cbor(rpc_result_t *rr, const char *method, size_t namelen,
                          uint8_t *cbor, size_t cborlen);
