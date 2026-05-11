#include "vllp_rpc.h"

#include "vllp.h"

#include <stdlib.h>
#include <string.h>

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



vllp_channel_t *
vllp_rpc_create(struct vllp *v)
{
  return vllp_channel_create(v, "rpc", 0, NULL, NULL, NULL, NULL);
}

static vllp_rpc_result_t *
vllp_int(char type, int i32)
{
  vllp_rpc_result_t *r = malloc(sizeof(vllp_rpc_result_t));
  r->type = type;
  r->i32 = i32;
  return r;
}

static vllp_rpc_result_t *
vllp_err(int err_code)
{
  return vllp_int('e', err_code);
}

static vllp_rpc_result_t *
vllp_intbuf(char type, const void *buf, size_t len)
{
  vllp_rpc_result_t *r = malloc(sizeof(vllp_rpc_result_t));
  if(len == 4) {
    r->type = type;
    memcpy(&r->i32, buf, 4);
  } else {
    r->type = 'e';
    r->i32 = VLLP_ERR_MALFORMED;
  }
  return r;
}


vllp_rpc_result_t *
vllp_rpc_invoke(vllp_channel_t *vc, const char *method,
                const uint8_t *args, size_t arglen,
                int timeout_seconds)
{
  size_t methodlen = strlen(method);
  size_t pktlen = 1 + methodlen + arglen;
  uint8_t *pkt = malloc(pktlen);
  pkt[0] = methodlen;
  memcpy(pkt + 1, method, methodlen);
  memcpy(pkt + 1 + methodlen, args, arglen);

  vllp_channel_send(vc, pkt, pktlen);

  void *buf;
  size_t buf_size;

  int result = vllp_channel_read(vc, &buf, &buf_size, timeout_seconds*1000*1000);
  if(result)
    return vllp_err(result);

  if(buf == NULL)
    return vllp_err(VLLP_ERR_NOT_CONNECTED);

  if(buf_size < 1) {
    free(buf);
    return vllp_err(VLLP_ERR_MALFORMED);
  }

  char type = *(const char *)buf;
  buf++;
  buf_size--;

  vllp_rpc_result_t *r;

  switch(type) {
  case 'e':
  case 'f':
  case 'i':
    return vllp_intbuf(type, buf, buf_size);
  case 's':
  case 'b':
    r = malloc(sizeof(vllp_rpc_result_t) + buf_size + 1);
    r->type = type;
    r->len = buf_size;
    memcpy(r->data, buf, buf_size);
    r->data[buf_size] = 0; // nul-terminate string
    return r;
  default:
    r = malloc(sizeof(vllp_rpc_result_t));
    r->type = type;
    return r;
  }
}
