#pragma once

#include "net/net_task.h"
#include "net/netif.h"

#define LLMTU (PBUF_DATA_SIZE - 2)

LIST_HEAD(l2cap_connection_list, l2cap_connection);

typedef struct l2cap {

  net_task_t l2c_task;

  struct netif *l2c_netif;

  struct pbuf_queue l2c_rx_queue;

  // Will never fail
  void (*l2c_output)(struct l2cap *self, struct pbuf *pb);

  struct l2cap_connection_list l2c_connections;

  struct pbuf_queue l2c_tx_queue;

  uint16_t l2c_tx_queue_len;
  uint16_t l2c_rx_queue_len;

} l2cap_t;

void l2cap_input(l2cap_t *l2c, pbuf_t *pb);

error_t l2cap_connect(l2cap_t *l2c);

void l2cap_disconnect(l2cap_t *l2c);

struct stream;
void l2cap_print(l2cap_t *l2c, struct stream *st);
