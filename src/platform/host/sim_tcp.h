#pragma once

/*
 * Scripted TCP peer for a vnet, for simulation threads (see
 * cpu/host/sim.h). It is not a TCP implementation: the script crafts
 * every segment itself and looks at every segment Mios sends, so it can
 * lose, reorder, duplicate and delay at will and check what Mios does
 * about it and when. Only ARP is answered behind the script's back.
 *
 * Sequence numbers in sim_tcp_seg_t and in the send helpers are
 * relative: 0 is the first data byte in that direction (ISN + 1), like
 * tcpdump prints them.
 */

#include <stdint.h>
#include <stddef.h>

#include "vnet.h"

#define SIM_TCP_FIN 0x01
#define SIM_TCP_SYN 0x02
#define SIM_TCP_RST 0x04
#define SIM_TCP_PSH 0x08
#define SIM_TCP_ACK 0x10

typedef struct sim_tcp_seg {
  uint64_t at;                 // When it was received
  uint32_t seq;                // Relative, except in a SYN (raw)
  uint32_t ack;                // Relative, 0 without ACK
  uint8_t flags;
  uint16_t wnd;                // Raw window field
  int len;                     // Payload bytes
  const uint8_t *data;         // Valid until the next receive
  int mss;                     // From the options, -1 if absent
  int wscale;                  // From the options, -1 if absent
} sim_tcp_seg_t;

typedef struct sim_tcp {
  vnet_t *vnet;

  // Configuration (network byte order addresses)
  uint8_t mac[6];
  uint32_t ip;                 // The peer's address
  uint32_t mios_ip;
  uint16_t port;               // The peer's port
  uint16_t mios_port;

  // Options the peer puts in its SYN or SYN-ACK
  int mss;                     // 0: no MSS option
  int wscale;                  // -1: no window scale option

  uint32_t isn;                // The peer's initial sequence number
  uint32_t mios_isn;           // Learned from Mios's SYN

  int rx_segments;             // Segments seen for this connection

  // Internals
  uint8_t mios_mac[6];
  uint16_t ip_id;
  uint8_t rxframe[2048];
  uint8_t txframe[2048];
} sim_tcp_t;

// Next segment Mios sends on this connection. Returns 0 if 'timeout'
// (us) passes first. Segments for other connections are ignored
int sim_tcp_recv(sim_tcp_t *t, sim_tcp_seg_t *seg, uint64_t timeout);

// Send a segment. seq and ack are relative
void sim_tcp_send(sim_tcp_t *t, int flags, uint32_t seq, uint32_t ack,
                  uint16_t wnd, const void *data, size_t len);

// Active open towards Mios: SYN, expect SYN-ACK (returned in synack if
// not NULL), ACK. Returns 1 on success
int sim_tcp_connect(sim_tcp_t *t, uint16_t wnd, sim_tcp_seg_t *synack);

// Passive open: wait for Mios's SYN (returned in syn if not NULL),
// answer SYN-ACK, expect the ACK. Returns 1 on success
int sim_tcp_accept(sim_tcp_t *t, uint16_t wnd, sim_tcp_seg_t *syn,
                   uint64_t timeout);
