#include "rtp_h264.h"

#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>
#include <sys/param.h>

#include <mios/task.h>
#include <mios/mios.h>
#include <stdio.h>

#include "net/pbuf.h"
#include "net/net_task.h"
#include "net/ipv4/ipv4.h"
#include "net/ipv4/udp.h"

// Room for ethernet + IPv4 + UDP headers in front of the RTP header
#define RTP_HEADROOM (16 + 20 + 8)
#define RTP_HDR_SIZE 12

// Ethernet MTU 1500 - IPv4 20 - UDP 8 - RTP 12, rounded down a bit to
// leave room for tunnels
#define RTP_MAX_PAYLOAD 1400

// Stop queueing when the free pbuf count drops below this, so the
// stream can't starve the receive path
#define RTP_PBUF_RESERVE 32

// How long to wait for the pool to drain before dropping the rest of a
// frame
#define RTP_MAX_WAIT_MS 50

// Locate the next start code at or after `pos`. Returns the offset of the
// first byte after the start code, or `len` if there is none.
static size_t
find_nal_start(const uint8_t *buf, size_t pos, size_t len)
{
  while(pos + 3 <= len) {
    if(buf[pos + 2] > 1) {
      pos += 3;
    } else if(buf[pos] == 0 && buf[pos + 1] == 0 && buf[pos + 2] == 1) {
      return pos + 3;
    } else {
      pos++;
    }
  }
  return len;
}


error_t
rtp_h264_packetize(const uint8_t *au, size_t len, size_t max_payload,
                   rtp_h264_emit_t *emit, void *opaque)
{
  size_t start = find_nal_start(au, 0, len);
  if(start == len)
    return ERR_MALFORMED;

  while(start < len) {
    size_t next = find_nal_start(au, start, len);
    size_t end = next == len ? len : next - 3;

    // Zero bytes before a start code belong to it (four byte form or
    // trailing_zero_8bits), not to the NAL unit
    while(end > start && au[end - 1] == 0)
      end--;

    const uint8_t *nal = au + start;
    size_t nal_len = end - start;
    const int last = next == len;
    error_t err;

    if(nal_len == 0) {
      // Empty NAL, skip
    } else if(nal_len <= max_payload) {
      err = emit(opaque, NULL, 0, nal, nal_len, last);
      if(err)
        return err;
    } else {
      // FU-A: the NAL header is replaced by the FU indicator (F, NRI and
      // type 28) and the FU header (S, E and the original type)
      uint8_t fu[2];
      fu[0] = (nal[0] & 0xe0) | 28;
      const uint8_t type = nal[0] & 0x1f;
      size_t pos = 1;
      const size_t chunk = max_payload - sizeof(fu);
      while(pos < nal_len) {
        const size_t n = MIN(chunk, nal_len - pos);
        fu[1] = type;
        if(pos == 1)
          fu[1] |= 0x80;
        if(pos + n == nal_len)
          fu[1] |= 0x40;
        err = emit(opaque, fu, sizeof(fu), nal + pos, n,
                   last && pos + n == nal_len);
        if(err)
          return err;
        pos += n;
      }
    }
    start = next;
  }
  return 0;
}


typedef struct rtp_h264 {
  video_sink_t rh_sink;

  net_task_t rh_task;

  mutex_t rh_mutex;
  struct pbuf_queue rh_queue;

  uint32_t rh_dst_addr;
  uint16_t rh_dst_port;
  uint16_t rh_src_port;

  uint16_t rh_seq;
  uint32_t rh_ssrc;
  uint32_t rh_ts;

  uint32_t rh_frames;
  uint32_t rh_keyframes;
  uint32_t rh_dropped;
  uint32_t rh_packets;
  uint64_t rh_bytes;
} rtp_h264_t;


static void
rtp_h264_net_cb(net_task_t *nt, uint32_t signals)
{
  rtp_h264_t *rh = (rtp_h264_t *)((char *)nt - offsetof(rtp_h264_t, rh_task));
  struct pbuf_queue q;

  mutex_lock(&rh->rh_mutex);
  STAILQ_INIT(&q);
  STAILQ_CONCAT(&q, &rh->rh_queue);
  mutex_unlock(&rh->rh_mutex);

  pbuf_t *pb;
  while((pb = STAILQ_FIRST(&q)) != NULL) {
    // Packets are queued as chains, the head of each has PBUF_SOP set
    pbuf_t *n = pb;
    while(!(n->pb_flags & PBUF_EOP))
      n = STAILQ_NEXT(n, pb_link);
    STAILQ_REMOVE_HEAD_UNTIL(&q, n, pb_link);
    n->pb_next = NULL;
    udp_send(NULL, pb, rh->rh_dst_addr, NULL, rh->rh_src_port,
             rh->rh_dst_port);
  }
}


// Copy `len` bytes to the end of a pbuf chain, growing it as needed.
// `*tailp` is the last buffer of the chain and is kept up to date.
static pbuf_t *
pbuf_chain_append(pbuf_t *head, pbuf_t **tailp, const uint8_t *data,
                  size_t len)
{
  pbuf_t *pb = *tailp;

  while(len) {
    size_t avail = PBUF_DATA_SIZE - pb->pb_offset - pb->pb_buflen;
    if(avail == 0) {
      pbuf_t *n = pbuf_make(0, 0);
      if(n == NULL) {
        pbuf_free(head);
        return NULL;
      }
      pb->pb_flags &= ~PBUF_EOP;
      n->pb_flags = PBUF_EOP;
      pb->pb_next = n;
      pb = n;
      continue;
    }
    size_t n = MIN(len, avail);
    memcpy(pb->pb_data + pb->pb_offset + pb->pb_buflen, data, n);
    pb->pb_buflen += n;
    head->pb_pktlen += n;
    data += n;
    len -= n;
  }
  *tailp = pb;
  return head;
}


// Hand queued packets to the net thread
static void
rtp_h264_flush(rtp_h264_t *rh, struct pbuf_queue *q)
{
  if(STAILQ_FIRST(q) == NULL)
    return;
  mutex_lock(&rh->rh_mutex);
  STAILQ_CONCAT(&rh->rh_queue, q);
  mutex_unlock(&rh->rh_mutex);
  net_task_raise(&rh->rh_task, 1);
}


typedef struct {
  rtp_h264_t *rh;
  struct pbuf_queue q;
  int dropped;
} rtp_send_ctx_t;


static error_t
rtp_h264_emit_packet(void *opaque, const uint8_t *hdr, size_t hdrlen,
                     const uint8_t *payload, size_t len, int marker)
{
  rtp_send_ctx_t *ctx = opaque;
  rtp_h264_t *rh = ctx->rh;

  // Large frames don't fit in the pbuf pool all at once. Hand over what
  // we have and give the net thread a chance to send it.
  int waited = 0;
  while(!ctx->dropped && pbuf_buffer_avail() < RTP_PBUF_RESERVE) {
    if(waited >= RTP_MAX_WAIT_MS) {
      ctx->dropped = 1;
      break;
    }
    rtp_h264_flush(rh, &ctx->q);
    usleep(1000);
    waited++;
  }

  // Once one packet is lost, the rest of the frame is dropped too, a
  // decoder can't do much with a partial slice anyway
  if(ctx->dropped) {
    rh->rh_seq++;
    return 0;
  }

  pbuf_t *pb = pbuf_make(RTP_HEADROOM, 0);
  if(pb == NULL) {
    ctx->dropped = 1;
    rh->rh_seq++;
    return 0;
  }

  uint8_t *rtp = pbuf_append(pb, RTP_HDR_SIZE);
  rtp[0] = 0x80;  // V=2
  rtp[1] = (marker ? 0x80 : 0) | RTP_H264_PAYLOAD_TYPE;
  rtp[2] = rh->rh_seq >> 8;
  rtp[3] = rh->rh_seq;
  rtp[4] = rh->rh_ts >> 24;
  rtp[5] = rh->rh_ts >> 16;
  rtp[6] = rh->rh_ts >> 8;
  rtp[7] = rh->rh_ts;
  rtp[8] = rh->rh_ssrc >> 24;
  rtp[9] = rh->rh_ssrc >> 16;
  rtp[10] = rh->rh_ssrc >> 8;
  rtp[11] = rh->rh_ssrc;
  rh->rh_seq++;
  rh->rh_packets++;
  rh->rh_bytes += RTP_HDR_SIZE + hdrlen + len;

  pbuf_t *tail = pb;
  if(hdrlen)
    pb = pbuf_chain_append(pb, &tail, hdr, hdrlen);
  if(pb != NULL)
    pb = pbuf_chain_append(pb, &tail, payload, len);
  if(pb == NULL) {
    ctx->dropped = 1;
    return 0;
  }

  // The chain is linked through pb_link already, so splice it in as is
  // (inserting the buffers one by one would cut the chain)
  struct pbuf_queue chain = { pb, &tail->pb_next };
  STAILQ_CONCAT(&ctx->q, &chain);
  return 0;
}


static error_t
rtp_h264_frame(video_sink_t *vs, const uint8_t *au, size_t len,
               uint64_t pts, int keyframe)
{
  rtp_h264_t *rh = (rtp_h264_t *)vs;
  rtp_send_ctx_t ctx = { .rh = rh };
  STAILQ_INIT(&ctx.q);

  rh->rh_ts = pts * 9 / 100; // 90 kHz
  rh->rh_frames++;
  if(keyframe)
    rh->rh_keyframes++;
  error_t err = rtp_h264_packetize(au, len, RTP_MAX_PAYLOAD,
                                   rtp_h264_emit_packet, &ctx);

  // Whatever made it into the queue is sent even if the frame was cut
  // short, the decoder can still use the complete NAL units
  rtp_h264_flush(rh, &ctx.q);
  if(ctx.dropped) {
    rh->rh_dropped++;
    return err ?: ERR_NO_BUFFER;
  }
  return err;
}


static void
rtp_h264_print_info(video_sink_t *vs, stream_t *st)
{
  rtp_h264_t *rh = (rtp_h264_t *)vs;
  stprintf(st, "  RTP/H.264 to %Id:%d  SSRC: 0x%08x\n",
           rh->rh_dst_addr, rh->rh_dst_port, rh->rh_ssrc);
  stprintf(st, "  Frames: %u (%u key, %u dropped)  Packets: %u  "
           "Bytes: %u kB\n",
           rh->rh_frames, rh->rh_keyframes, rh->rh_dropped,
           rh->rh_packets, (uint32_t)(rh->rh_bytes / 1024));
}


static void
rtp_h264_close(video_sink_t *vs)
{
  rtp_h264_t *rh = (rtp_h264_t *)vs;

  // Give the net thread a chance to send what's queued
  for(int i = 0; i < 100 && STAILQ_FIRST(&rh->rh_queue) != NULL; i++)
    usleep(1000);

  net_task_cancel(&rh->rh_task);
  // Whatever is still queued is ours to free
  mutex_lock(&rh->rh_mutex);
  pbuf_t *pb;
  while((pb = STAILQ_FIRST(&rh->rh_queue)) != NULL) {
    STAILQ_REMOVE_HEAD(&rh->rh_queue, pb_link);
    pb->pb_next = NULL;
    pbuf_free(pb);
  }
  mutex_unlock(&rh->rh_mutex);
  free(rh);
}


video_sink_t *
rtp_h264_create(uint32_t dst_addr, uint16_t dst_port)
{
  rtp_h264_t *rh = calloc(1, sizeof(rtp_h264_t));
  rh->rh_sink.frame = rtp_h264_frame;
  rh->rh_sink.print_info = rtp_h264_print_info;
  rh->rh_sink.close = rtp_h264_close;
  rh->rh_task.nt_cb = rtp_h264_net_cb;
  mutex_init(&rh->rh_mutex, "rtp");
  STAILQ_INIT(&rh->rh_queue);
  rh->rh_dst_addr = dst_addr;
  rh->rh_dst_port = dst_port;
  rh->rh_src_port = dst_port;
  rh->rh_ssrc = rand();
  rh->rh_seq = rand();
  return &rh->rh_sink;
}
