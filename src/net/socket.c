#include "irq.h"
#include "socket.h"

#include "ipv4.h"
#include "udp.h"

static error_t
socket_net_attach(socket_t *s, socket_ctl_t *sc)
{
  switch(s->s_protocol) {
  case IPPROTO_UDP:
    return udp_socket_attach(s);
  default:
    return ERR_NOT_IMPLEMENTED;
  }
}


static error_t
socket_net_detach(socket_t *s, socket_ctl_t *sc)
{
  switch(s->s_protocol) {
  case IPPROTO_UDP:
    return udp_socket_detach(s);
  default:
    return ERR_NOT_IMPLEMENTED;
  }
}


error_t
socket_net_ctl(socket_t *s, socket_ctl_t *sc)
{
  switch(sc->sc_op) {
  case SOCKET_CTL_ATTACH:
    return socket_net_attach(s, sc);
  case SOCKET_CTL_DETACH:
    return socket_net_detach(s, sc);
  }
  return ERR_NOT_IMPLEMENTED;
}


void
socket_init(socket_t *s)
{
  STAILQ_INIT(&s->s_tx_queue);
  STAILQ_INIT(&s->s_op_queue);
}
