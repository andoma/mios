#if 0
#include "socket.h"

#include <string.h>

#include "irq.h"

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

error_t
socket_sendv(socket_t *s, const struct iovec *iov, size_t iovcnt, int flags)
{
  int wait = !(flags & SOCK_SEND_NONBLOCK);
  pbuf_t *pb = pbuf_make(s->s_header_size, wait);
  if(pb == NULL)
    return ERR_NO_BUFFER;

  if(flags & SOCK_SEND_BROADCAST)
    pb->pb_flags |= PBUF_BCAST;

  for(size_t i = 0; i < iovcnt; i++) {
    void *dst = pbuf_append(pb, iov[i].iov_len);
    memcpy(dst, iov[i].iov_base, iov[i].iov_len);
  }

  int q = irq_forbid(IRQ_LEVEL_NET);
  net_wakeup_socket(s);
  STAILQ_INSERT_TAIL(&s->s_tx_queue, pb, pb_link);
  irq_permit(q);
  return 0;
}

error_t
socket_send(socket_t *s, const void *data, size_t len, int flags)
{
  struct iovec iov = {(void *)data, len};
  return socket_sendv(s, &iov, 1, flags);
}
#endif
