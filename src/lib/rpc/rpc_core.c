#include "rpc_core.h"

#include <mios/version.h>

#include <string.h>

static error_t
rpc_ping(rpc_result_t *rr)
{
  rr->type = RPC_TYPE_VOID;
  return 0;
}

RPC_DEF("ping()", rpc_ping);

static error_t
rpc_buildid(rpc_result_t *rr)
{
  rr->type = RPC_TYPE_CONST_BINARY;
  rr->data = (void *)mios_build_id();
  rr->size = 20;
  return 0;
}

RPC_DEF("buildid()", rpc_buildid);

static error_t
rpc_appname(rpc_result_t *rr)
{
  rr->type = RPC_TYPE_CONST_STRING;
  rr->data = (void *)mios_get_app_name();
  return 0;
}

RPC_DEF("appname()", rpc_appname);






static error_t
rpc_add(rpc_result_t *rr, int a, int b)
{
  rr->type = RPC_TYPE_INT;
  rr->i32 = a + b;
  return 0;
}

RPC_DEF("add(ii)", rpc_add);

static error_t
rpc_fadd(rpc_result_t *rr, float a, float b)
{
  rr->type = RPC_TYPE_FLOAT;
  rr->flt = a + b;
  return 0;
}

RPC_DEF("fadd(ff)", rpc_fadd);

static error_t
rpc_combo(rpc_result_t *rr, float a, int b, int c, float d)
{
  rr->type = RPC_TYPE_FLOAT;
  rr->flt = a + b + c + d;
  return 0;
}

RPC_DEF("combo(fiif)", rpc_combo);




const rpc_method_t *
rpc_method_resovle(const char *name, size_t namelen)
{
  extern unsigned long _rpcdef_array_begin;
  extern unsigned long _rpcdef_array_end;

  const rpc_method_t *rm = (void *)&_rpcdef_array_begin;
  for(; rm != (const void *)&_rpcdef_array_end; rm++) {
    size_t l = strlen(rm->signature);
    if(l < namelen)
      continue;
    if(!memcmp(name, rm->signature, namelen) && rm->signature[namelen] == '(') {
      return rm;
    }
  }
  return NULL;
}

error_t
rpc_trampoline(rpc_result_t *rr, const rpc_args_t *ra, const rpc_method_t *rm)
{
#ifdef RPC_ARGS_MAX_FLT
  error_t (*fn)(rpc_result_t *rr, long l1, long l2, long l3,
                float f1, float f2, float f3, float f4) = rm->invoke;

  return fn(rr, ra->gpr[0], ra->gpr[1], ra->gpr[2],
            ra->flt[0], ra->flt[1], ra->flt[2], ra->flt[3]);

#else
  error_t (*fn)(rpc_result_t *rr, long l1, long l2, long l3) = rm->invoke;
  return fn(rr, ra->gpr[0], ra->gpr[1], ra->gpr[2]);
#endif
}

