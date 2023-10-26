#include "rpc_core.h"

#include <string.h>

#ifdef RPC_ARGS_MAX_FLT
static int
cbor_decode_float(const uint8_t *cbor, size_t cborlen, float *store)
{
  if(*cbor == 0xfa) {
    if(cborlen < 5)
      return ERR_INVALID_RPC_ARGS;

    uint32_t u32;
    memcpy(&u32, cbor + 1, 4);
#if !defined(__BIG_ENDIAN__)
    u32 = __builtin_bswap32(u32);
#endif
    memcpy(store, &u32, 4);
    return 5;
  }
  return ERR_INVALID_ARGS;
}
#endif

static int
cbor_decode_scalar(const uint8_t *cbor, size_t cborlen, int *store, int neg)
{
  int v = *cbor & 0x1f;
  int r = 1;

  if(v > 0x1a)
    return ERR_INVALID_ARGS;
  if(v >= 0x18) {
    int bytes = 1;
    if(v == 0x19)
      bytes = 2;
    else if(v == 0x1a)
      bytes = 4;

    r += bytes;
    if(r > cborlen)
      return ERR_INVALID_ARGS;

    v = 0;
    for(size_t i = 0; i < bytes; i++) {
      v = (v << 8) | cbor[i + 1];
    }
  }

  if(neg)
    v = ~v;
  *store = v;
  return r;
}


static int
cbor_decode_int(const uint8_t *cbor, size_t cborlen, int *store)
{
  return cbor_decode_scalar(cbor, cborlen, store, *cbor & 0x20);
}


static int
rpc_cbor_get_int(uint8_t *cbor, size_t cborlen, rpc_args_t *ra)
{
  if(ra->num_gpr == RPC_ARGS_MAX_GPR)
    return ERR_INVALID_RPC_ARGS;

#ifdef RPC_ARGS_MAX_FLT
  if(*cbor > 0xf9) {
    float f;
    int r = cbor_decode_float(cbor, cborlen, &f);
    ra->gpr[ra->num_gpr++] = f;
    return r;
  }
#endif

  if(*cbor >= 0x40)
    return ERR_INVALID_RPC_ARGS;

  int i32;
  int r = cbor_decode_int(cbor, cborlen, &i32);
  ra->gpr[ra->num_gpr++] = i32;
  return r;
}



#ifdef RPC_ARGS_MAX_FLT
static int
rpc_cbor_get_float(uint8_t *cbor, size_t cborlen, rpc_args_t *ra)
{
  if(ra->num_flt == RPC_ARGS_MAX_FLT)
    return ERR_INVALID_RPC_ARGS;

  if(*cbor > 0xf9) {
    return cbor_decode_float(cbor, cborlen, &ra->flt[ra->num_flt++]);
  }

  if(*cbor >= 0x40)
    return ERR_INVALID_RPC_ARGS;

  int i32;
  int r = cbor_decode_int(cbor, cborlen, &i32);
  ra->flt[ra->num_flt++] = i32;
  return r;
}
#endif


static int
rpc_cbor_get_utf8(uint8_t *cbor, size_t cborlen, rpc_args_t *ra)
{
  if(ra->num_gpr == RPC_ARGS_MAX_GPR)
    return ERR_INVALID_RPC_ARGS;

  int len;
  int used = cbor_decode_scalar(cbor, cborlen, &len, 0);
  if(used < 0)
    return used;

  if(used + len > cborlen)
    return ERR_INVALID_RPC_ARGS;

  memmove(cbor, cbor + used, len);
  cbor[len] = 0;
  ra->gpr[ra->num_gpr++] = (intptr_t)cbor;
  return used + len;
}



static int
rpc_cbor_get_bytestring(uint8_t *cbor, size_t cborlen, rpc_args_t *ra)
{
  if(ra->num_gpr + 1 >= RPC_ARGS_MAX_GPR)
    return ERR_INVALID_RPC_ARGS;

  int len;
  int used = cbor_decode_scalar(cbor, cborlen, &len, 0);
  if(used < 0)
    return used;

  if(used + len > cborlen)
    return ERR_INVALID_RPC_ARGS;

  ra->gpr[ra->num_gpr++] = (intptr_t)(cbor + used);
  ra->gpr[ra->num_gpr++] = len;
  return used + len;
}



#include <stdio.h>

error_t
rpc_dispatch_cbor(rpc_result_t *rr, const char *method, size_t namelen,
                  uint8_t *cbor, size_t cborlen)
{
  int cborargcount = 0;

  ///  hexdump("CBOR", cbor, cborlen);

  if(cborlen) {
    // Expect array
    if((*cbor >> 5) != 4)
      return ERR_INVALID_RPC_ARGS;

    if(*cbor == 0x9f) {
      cborargcount = INT32_MAX;

      cbor++;
      cborlen--;

    } else {

      int used = cbor_decode_scalar(cbor, cborlen, &cborargcount, 0);
      if(used < 0)
        return used;

      cbor += used;
      cborlen -= used;
    }
  }

  const rpc_method_t *rm = rpc_method_resovle(method, namelen);
  if(rm == NULL) {
    return ERR_NOT_FOUND;
  }

  const char *at = rm->signature + namelen + 1; // Points past '('

  rpc_args_t ra = {};

  while(*at >= 'a') {
    if(!cborlen)
      return ERR_INVALID_RPC_ARGS;
    int r;
    switch(*at) {
    case 'i':
      r = rpc_cbor_get_int(cbor, cborlen, &ra);
      break;
#ifdef RPC_ARGS_MAX_FLT
    case 'f':
      r = rpc_cbor_get_float(cbor, cborlen, &ra);
      break;
#endif
    case 's':
      r = rpc_cbor_get_utf8(cbor, cborlen, &ra);
      break;
    case 'b':
      r = rpc_cbor_get_bytestring(cbor, cborlen, &ra);
      break;
    default:
      return ERR_INVALID_RPC_ARGS;
    }
    if(r < 0)
      return r;
    at++;
    cbor += r;
    cborlen -= r;
  }

  if(cborlen) {
    if(!(cborlen == 1 && *cbor == 0xff)) {
      return ERR_INVALID_RPC_ARGS;
    }
  }

  return rpc_trampoline(rr, &ra, rm);
}
