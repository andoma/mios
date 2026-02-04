#include "ptp.h"

#include <mios/eventlog.h>

#include <net/net.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <inttypes.h>

#include "ether.h"

#define MSGTYPE_SYNC          0
#define MSGTYPE_DELAY_REQ     1
#define MSGTYPE_FOLLOWUP      8
#define MSGTYPE_DELAY_RESP    9
#define MSGTYPE_ANNOUNCE      11

#define PTP_TIMESCALE         0x0008
#define PTP_FLAG_TWO_STEP     0x0200

static const uint8_t ptp_mc_addr[6] = { 0x01, 0x1B, 0x19, 0x00, 0x00, 0x00 };

struct ptp_hdr {
  uint8_t  message_type : 4;
  uint8_t  major_sdo_id : 4;
  uint8_t  version;
  uint16_t msg_len;
  uint8_t  domain;
  uint8_t  reserved1;
  uint16_t flags;
  int64_t  correction;
  uint32_t message_specific;
  uint8_t clock_identity[8];

  uint16_t source_port_id;
  uint16_t sequence_id;
  uint8_t  control;
  int8_t   log_message_interval;

  uint16_t timestamp_seconds_hi;
  uint32_t timestamp_seconds_low;
  uint32_t timestamp_nanoseconds;

} __attribute__((packed));

#define PTP_NSEC_PER_SEC 1000000000LL

static int64_t
ptp_ts_to_ns(const ptp_ts_t *ts)
{
  return (int64_t)ts->seconds * PTP_NSEC_PER_SEC + ts->nanoseconds;
}


static void
calc(ether_netif_t *eni)
{
  ptp_ether_state_t *pes = &eni->eni_ptp;

  const int64_t t1 = ptp_ts_to_ns(&pes->pes_t1);
  const int64_t t2 = ptp_ts_to_ns(&pes->pes_t2);
  const int64_t t3 = ptp_ts_to_ns(&pes->pes_t3);
  const int64_t t4 = ptp_ts_to_ns(&pes->pes_t4);

  const int64_t d_ms = t2 - t1 - (pes->pes_t1_cf >> 16);
  const int64_t d_sm = t4 - t3 - (pes->pes_t4_cf >> 16);

  const int64_t one_way_delay = (d_sm + d_ms) / 2;
  const int64_t offset        = (d_sm - d_ms) / 2;

  if(eni->eni_adjust_mac_clock != NULL)
    eni->eni_adjust_mac_clock(eni, offset);

  pes->pes_offset = offset;
  pes->pes_one_way_delay = one_way_delay;
}



static void
ptpv2_tx_delay_req_tx_cb(struct netif *ni,
                         const struct pbuf_timestamp *pt)
{
  ether_netif_t *eni = (ether_netif_t *)ni;
  ptp_ether_state_t *pes = &eni->eni_ptp;

  if(pt->pt_id != pes->pes_delay_req_seq)
    return;

  pes->pes_t3.seconds = pt->pt_seconds;
  pes->pes_t3.nanoseconds = pt->pt_nanoseconds;
}


static pbuf_t *
ptpv2_send_delay_req(ether_netif_t *eni, pbuf_t *pb,
                     struct ptp_hdr *p, ether_hdr_t *eh)
{
  ptp_ether_state_t *pes = &eni->eni_ptp;
  pes->pes_delay_req_seq++;

  // Recycle pb
  memcpy(eh->dst_addr, ptp_mc_addr, 6);
  memcpy(eh->src_addr, eni->eni_addr, 6);

  p->message_type = MSGTYPE_DELAY_REQ;
  p->correction = 0;
  p->message_specific = 0;
  memcpy(p->clock_identity, eni->eni_addr, 3);
  p->clock_identity[3] = 0xff;
  p->clock_identity[4] = 0xfe;
  memcpy(p->clock_identity + 5, eni->eni_addr + 3, 3);
  p->source_port_id = htons(1);
  p->sequence_id = htons(pes->pes_delay_req_seq);
  p->control = 1;
  p->log_message_interval = 0x7f;
  p->timestamp_seconds_hi = 0;
  p->timestamp_seconds_low = 0;
  p->timestamp_nanoseconds = 0;

  eni->eni_output(eni, pb, ptpv2_tx_delay_req_tx_cb, pes->pes_delay_req_seq);
  return NULL;
}


static pbuf_t *
ptpv2_handle_followup(ether_netif_t *eni, pbuf_t *pb, pbuf_timestamp_t *pt,
                      struct ptp_hdr *p, ether_hdr_t *eh)
{
  ptp_ether_state_t *pes = &eni->eni_ptp;

  if(pes->pes_sync_seq != p->sequence_id)
    return pb;

  pes->pes_t1.seconds = ntohl(p->timestamp_seconds_low);
  pes->pes_t1.nanoseconds = ntohl(p->timestamp_nanoseconds);
  pes->pes_t1_cf += __builtin_bswap64(p->correction);

  return ptpv2_send_delay_req(eni, pb, p, eh);
}


static pbuf_t *
ptpv2_handle_sync(ether_netif_t *eni, pbuf_t *pb, pbuf_timestamp_t *pt,
                  struct ptp_hdr *p, ether_hdr_t *eh)
{
  ptp_ether_state_t *pes = &eni->eni_ptp;

  if(pt == NULL)
    return pb; // We rely on hardware timestamps

  pes->pes_t2.seconds = pt->pt_seconds;
  pes->pes_t2.nanoseconds = pt->pt_nanoseconds;
  pes->pes_t1_cf = __builtin_bswap64(p->correction);
  pes->pes_sync_seq = p->sequence_id;

  if(p->flags & ntohs(PTP_FLAG_TWO_STEP))
    return pb;

  p->correction = 0; // Accounted for already, don't add twice
  return ptpv2_handle_followup(eni, pb, pt, p, eh);
}


static pbuf_t *
ptpv2_handle_delay_resp(ether_netif_t *eni, pbuf_t *pb, pbuf_timestamp_t *pt,
                        struct ptp_hdr *p, ether_hdr_t *eh)
{
  ptp_ether_state_t *pes = &eni->eni_ptp;

  pes->pes_t4.seconds = ntohl(p->timestamp_seconds_low);
  pes->pes_t4.nanoseconds = ntohl(p->timestamp_nanoseconds);
  pes->pes_t4_cf = __builtin_bswap64(p->correction);
  calc(eni);
  return pb;
}



pbuf_t *
ptpv2_input(ether_netif_t *eni, pbuf_t *pb, pbuf_timestamp_t *pt,
            size_t ether_header_size)
{
  if(pbuf_pullup(pb, pb->pb_pktlen))
    return pb;

  ether_hdr_t *eh = pbuf_data(pb, 0);
  struct ptp_hdr *p = pbuf_data(pb, ether_header_size);

  switch(p->message_type) {
  case MSGTYPE_SYNC:
    return ptpv2_handle_sync(eni, pb, pt, p, eh);
  case MSGTYPE_FOLLOWUP:
    return ptpv2_handle_followup(eni, pb, pt, p, eh);
  case MSGTYPE_DELAY_RESP:
    return ptpv2_handle_delay_resp(eni, pb, pt, p, eh);
  default:
    return pb;
  }
}


void
ptp_print_info(stream_t *st, struct ether_netif *eni)
{
  const ptp_ether_state_t *pes = &eni->eni_ptp;
  stprintf(st, "\tPTP Offset:%"PRId64" ns  Delay:%d ns\n", pes->pes_offset,
           pes->pes_one_way_delay);
  stprintf(st, "\tDownstream delay %"PRId64" ns\n", pes->pes_t1_cf >> 16);
  stprintf(st, "\tUpstream   delay %"PRId64" ns\n", pes->pes_t4_cf >> 16);
}
