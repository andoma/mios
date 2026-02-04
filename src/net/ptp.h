#pragma once

#include <stddef.h>
#include <stdint.h>

struct pbuf;
struct pbuf_timestamp;
struct ether_netif;
struct stream;

struct pbuf *ptpv2_input(struct ether_netif *eni,
                         struct pbuf *pb,
                         struct pbuf_timestamp *pt,
                         size_t ether_header_size);


typedef struct ptp_ts {
  uint32_t seconds;
  uint32_t nanoseconds;
} ptp_ts_t;


typedef struct ptp_ether_state {

  ptp_ts_t pes_t1;
  int64_t pes_t1_cf;
  ptp_ts_t pes_t2;
  uint16_t pes_sync_seq;
  uint16_t pes_delay_req_seq;

  ptp_ts_t pes_t3;
  ptp_ts_t pes_t4;
  int64_t pes_t4_cf;

  int64_t pes_offset;
  int pes_one_way_delay;

} ptp_ether_state_t;

void ptp_print_info(struct stream *st, struct ether_netif *eni);
