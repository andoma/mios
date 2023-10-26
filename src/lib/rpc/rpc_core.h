#pragma once

#include <mios/rpc.h>


#ifdef __arm__
#define RPC_ARGS_MAX_GPR 3

#ifdef __ARM_FP
#define RPC_ARGS_MAX_FLT 4
#endif

#else
#error Unsupported arch for RPC
#endif

const rpc_method_t *rpc_method_resovle(const char *name, size_t namelen);


typedef struct rpc_args {
  long gpr[RPC_ARGS_MAX_GPR];
  uint8_t num_gpr;

#ifdef RPC_ARGS_MAX_FLT
  float flt[RPC_ARGS_MAX_FLT];
  uint8_t num_flt;
#endif

} rpc_args_t;


error_t rpc_trampoline(rpc_result_t *rr, const rpc_args_t *ra,
                       const rpc_method_t *rm);
