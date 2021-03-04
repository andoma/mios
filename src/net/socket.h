#pragma once

#include <mios/task.h>
#include <mios/error.h>

#include "pbuf.h"

STAILQ_HEAD(socket_ctl_queue, socket_ctl);
STAILQ_HEAD(socket_queue, socket);
LIST_HEAD(socket_list, socket);



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

  LIST_ENTRY(socket) s_net_link;

  struct pbuf *(*s_rx)(struct socket *s, struct pbuf *pb);

  struct pbuf_queue s_tx_queue;
  STAILQ_ENTRY(socket) s_tx_link;

  struct socket_ctl_queue s_op_queue;
  STAILQ_ENTRY(socket) s_op_link;

  uint32_t s_remote_addr;
  uint32_t s_local_addr;

  uint16_t s_remote_port;
  uint16_t s_local_port;

  uint8_t s_protocol;
  uint8_t s_net_state;

} socket_t;

error_t socket_attach(socket_t *s);

error_t socket_detach(socket_t *s);

error_t socket_net_ctl(socket_t *s, socket_ctl_t *sc);
