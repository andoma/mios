#pragma once

#include <mios/task.h>
#include <mios/error.h>
#include <mios/mios.h>
#include <sys/uio.h>

#include "pbuf.h"

STAILQ_HEAD(socket_ctl_queue, socket_ctl);
STAILQ_HEAD(socket_queue, socket);
LIST_HEAD(socket_list, socket);

#define AF_INET 1
#define AF_MBUS 2

typedef enum {

  SOCKET_CTL_ATTACH = 1,
  SOCKET_CTL_DETACH,

} socket_ctl_op_t;



typedef struct socket_ctl {

  STAILQ_ENTRY(socket_ctl) sc_link;
  task_waitable_t sc_waitq;
  int sc_op;
  error_t sc_result;

} socket_ctl_t;



typedef struct socket {

  LIST_ENTRY(socket) s_proto_link;

  LIST_ENTRY(socket) s_netif_link;
  struct netif *s_netif;

  const struct socket_proto *s_proto;

  struct pbuf *(*s_rx)(struct socket *s, struct pbuf *pb);

  struct pbuf_queue s_tx_queue;
  struct socket_ctl_queue s_op_queue;
  STAILQ_ENTRY(socket) s_work_link;

  uint32_t s_remote_addr;
  uint32_t s_local_addr;

  uint16_t s_remote_port;
  uint16_t s_local_port;

  uint8_t s_net_state;
  uint8_t s_header_size;
  uint16_t s_mtu;

} socket_t;

void socket_init(socket_t *s, uint8_t family, uint8_t protocol);

error_t socket_attach(socket_t *s);

error_t socket_detach(socket_t *s);

error_t socket_net_ctl(socket_t *s, socket_ctl_t *sc);

error_t socket_send(socket_t *s, const void *data, size_t len, int flags);

error_t socket_sendv(socket_t *s, const struct iovec *iov,
                     size_t iovcnt, int flags);

#define SOCK_NONBLOCK 0x1

typedef struct socket_proto {
  uint8_t sp_family;
  uint8_t sp_protocol;
  error_t (*sp_ctl)(socket_t *s, socket_ctl_t *sc);
  pbuf_t *(*sp_send_async)(socket_t *s, pbuf_t *pb);
} socket_proto_t;

#define NET_SOCKET_PROTO_DEF(family, protocol, ctl, send)               \
  static socket_proto_t MIOS_JOIN(sockfam, __LINE__) __attribute__ ((used, section("netsock"))) = { family, protocol, ctl, send };

void net_wakeup_socket(struct socket *s);
