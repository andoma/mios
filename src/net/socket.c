#include "irq.h"
#include "socket.h"

#include "ipv4/ipv4.h"
#include "ipv4/udp.h"


static const socket_proto_t *
socket_proto_find(uint8_t family, uint8_t proto)
{
  extern unsigned long _netsock_array_begin;
  extern unsigned long _netsock_array_end;

  socket_proto_t *sp = (void *)&_netsock_array_begin;
  socket_proto_t *e = (void *)&_netsock_array_end;

  for(; sp != e; sp++) {
    if(sp->sp_family == family && sp->sp_protocol == proto)
      return sp;
  }
  return NULL;
}

error_t
socket_net_ctl(socket_t *s, socket_ctl_t *sc)
{
  if(s->s_proto == NULL)
    return ERR_NOT_IMPLEMENTED;

  return s->s_proto->sp_ctl(s, sc);
}


void
socket_init(socket_t *s, uint8_t family, uint8_t proto)
{
  STAILQ_INIT(&s->s_tx_queue);
  STAILQ_INIT(&s->s_op_queue);
  s->s_proto = socket_proto_find(family, proto);
}
