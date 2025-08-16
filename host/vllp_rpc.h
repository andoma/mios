#pragma once

#include <stdint.h>
#include <stddef.h>

struct vllp;

struct vllp_channel;

#define VLLP_RPC_TYPE_INT     'i'
#define VLLP_RPC_TYPE_FLOAT   'f'
#define VLLP_RPC_TYPE_STRING  's'
#define VLLP_RPC_TYPE_BINARY  'b'
#define VLLP_RPC_TYPE_VOID    'v'
#define VLLP_RPC_TYPE_ERROR   'e'


typedef struct vllp_rpc_result {
  char type;

  union {
    int i32;
    float flt;
    struct {
      size_t len;
      uint8_t data[0];
    };
  };
} vllp_rpc_result_t;

struct vllp_channel *vllp_rpc_create(struct vllp *v);

vllp_rpc_result_t *vllp_rpc_invoke(struct vllp_channel *vc, const char *method,
                                   const uint8_t *cbor, size_t len,
                                   int timeout_in_seconds);
