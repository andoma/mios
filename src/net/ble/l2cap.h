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

  // Reply to a pending controller LTK request (peripheral). ltk NULL rejects.
  // NULL when the controller has no link encryption (native link layer).
  void (*l2c_ltk_reply)(struct l2cap *self, const uint8_t *ltk);

  struct l2cap_connection_list l2c_connections;

  struct pbuf_queue l2c_tx_queue;

  uint16_t l2c_tx_queue_len;
  uint16_t l2c_rx_queue_len;

  // Link addresses, filled by the driver at connection setup; needed by the
  // pairing crypto. Stored least-significant-byte first (HCI order).
  uint8_t l2c_our_addr[6];
  uint8_t l2c_peer_addr[6];
  uint8_t l2c_our_addr_type;  // 0 = public, 1 = random
  uint8_t l2c_peer_addr_type;

  uint8_t l2c_sec_level; // achieved link security, BLE_SEC_* (0 until encrypted)
  uint8_t l2c_pending_sec_level; // level to apply when encryption turns on

  struct smp *l2c_smp; // pairing state, allocated on first SMP PDU

} l2cap_t;

void l2cap_output(l2cap_t *l2c, struct pbuf *pb, uint16_t cid);

// l2cap task signal raised by the driver when the link becomes encrypted.
#define L2CAP_SIGNAL_SMP 0x4

void l2cap_input(l2cap_t *l2c, pbuf_t *pb);

error_t l2cap_connect(l2cap_t *l2c);

void l2cap_disconnect(l2cap_t *l2c);

struct stream;
void l2cap_print(l2cap_t *l2c, struct stream *st);
