/*
 * "rtp-h264": RFC 6184 packetization of Annex-B access units.
 *
 * Feeds rtp_h264_packetize() access units with both start code forms
 * and trailing zero bytes, and checks that every NAL unit comes out
 * either as a single NAL unit packet or as FU-A fragments that
 * reassemble to exactly the original NAL, with the marker bit on the
 * last packet only.
 */

#include <string.h>
#include <stdint.h>

#include "net/rtp_h264.h"
#include "net/pbuf.h"

#include "hosttest.h"

#define MAX_PKTS 64
#define MAX_PAYLOAD 100

typedef struct {
  int count;
  int marker_at;
  int markers;
  size_t len[MAX_PKTS];
  uint8_t data[MAX_PKTS][MAX_PAYLOAD];
} capture_t;


static error_t
capture_emit(void *opaque, const uint8_t *hdr, size_t hdrlen,
             const uint8_t *payload, size_t len, int marker)
{
  capture_t *c = opaque;
  if(c->count == MAX_PKTS || hdrlen + len > MAX_PAYLOAD)
    return ERR_NO_BUFFER;
  memcpy(c->data[c->count], hdr, hdrlen);
  memcpy(c->data[c->count] + hdrlen, payload, len);
  c->len[c->count] = hdrlen + len;
  if(marker) {
    c->markers++;
    c->marker_at = c->count;
  }
  c->count++;
  return 0;
}


// Reassemble packet(s) starting at *idx into one NAL unit
static size_t
reassemble(const capture_t *c, int *idx, uint8_t *out)
{
  const uint8_t *p = c->data[*idx];
  if((p[0] & 0x1f) != 28) {
    memcpy(out, p, c->len[*idx]);
    return c->len[(*idx)++];
  }
  if(!(p[1] & 0x80))
    return 0; // First fragment must have S set
  out[0] = (p[0] & 0xe0) | (p[1] & 0x1f);
  size_t len = 1;
  while(*idx < c->count) {
    p = c->data[*idx];
    memcpy(out + len, p + 2, c->len[*idx] - 2);
    len += c->len[*idx] - 2;
    (*idx)++;
    if(p[1] & 0x40)
      return len;
  }
  return 0;
}


static int
pool_restored(void *arg)
{
  return pbuf_buffer_avail() == *(int *)arg;
}


/*
 * Push large access units through the RTP video sink. Every packet spans
 * several pbufs, which have to reach the net thread as intact chains
 * (it walks them to the end-of-packet marker). There is no route to the
 * destination, so udp_send() drops them, and the pool must refill.
 */
static int
run_send(void)
{
  int fails = 0;
  static uint8_t au[20000];
  memset(au, 0x55, sizeof(au));
  au[0] = 0; au[1] = 0; au[2] = 0; au[3] = 1; au[4] = 0x65;

  int avail = pbuf_buffer_avail();
  video_sink_t *vs = rtp_h264_create(0x0a000001, 5000);
  for(int i = 0; i < 5; i++) {
    error_t err = vs->frame(vs, au, sizeof(au), i * 33333, i == 0);
    fails += !CHECK(err == 0, "send %d failed: %d", i, err);
  }
  fails += !CHECK(hosttest_wait(pool_restored, &avail, 1000000),
                  "pbufs leaked: %d of %d available",
                  pbuf_buffer_avail(), avail);
  vs->close(vs);
  return fails;
}


static int
run_rtp_h264(void)
{
  int fails = 0;
  static uint8_t au[1024];
  size_t len = 0;

  static const uint8_t sps[] = { 0x67, 0x42, 0xc0, 0x1e, 0xda, 0x02 };
  static const uint8_t pps[] = { 0x68, 0xce, 0x3c, 0x80 };
  static uint8_t idr[350];
  idr[0] = 0x65;
  for(size_t i = 1; i < sizeof(idr); i++)
    idr[i] = i * 7 + 3; // Contains 0x00 and 0x01 but never 00 00 01

  // 4-byte start code, SPS, 3-byte start code, PPS followed by a
  // trailing zero, 4-byte start code, IDR followed by trailing zeros
  static const uint8_t sc4[] = { 0, 0, 0, 1 };
  memcpy(au + len, sc4, 4); len += 4;
  memcpy(au + len, sps, sizeof(sps)); len += sizeof(sps);
  memcpy(au + len, sc4 + 1, 3); len += 3;
  memcpy(au + len, pps, sizeof(pps)); len += sizeof(pps);
  au[len++] = 0;
  memcpy(au + len, sc4, 4); len += 4;
  memcpy(au + len, idr, sizeof(idr)); len += sizeof(idr);
  au[len++] = 0;
  au[len++] = 0;

  static capture_t c;
  memset(&c, 0, sizeof(c));
  error_t err = rtp_h264_packetize(au, len, MAX_PAYLOAD, capture_emit, &c);
  fails += !CHECK(err == 0, "packetize failed: %d", err);

  // SPS + PPS + ceil(349 / 98) fragments
  fails += !CHECK(c.count == 2 + 4, "got %d packets", c.count);
  fails += !CHECK(c.markers == 1 && c.marker_at == c.count - 1,
                  "marker on %d packets, last at %d",
                  c.markers, c.marker_at);

  int idx = 0;
  static uint8_t nal[sizeof(idr)];
  size_t n;

  n = reassemble(&c, &idx, nal);
  fails += !CHECK(n == sizeof(sps) && !memcmp(nal, sps, n), "SPS mismatch");
  n = reassemble(&c, &idx, nal);
  fails += !CHECK(n == sizeof(pps) && !memcmp(nal, pps, n), "PPS mismatch");
  n = reassemble(&c, &idx, nal);
  fails += !CHECK(n == sizeof(idr) && !memcmp(nal, idr, n),
                  "IDR mismatch (%zd bytes)", n);
  fails += !CHECK(idx == c.count, "%d packets left over", c.count - idx);

  for(int i = 0; i < c.count; i++)
    fails += !CHECK(c.len[i] <= MAX_PAYLOAD, "packet %d is %zd bytes",
                    i, c.len[i]);

  // A NAL that fits exactly is not fragmented
  memset(&c, 0, sizeof(c));
  len = 0;
  memcpy(au + len, sc4 + 1, 3); len += 3;
  memcpy(au + len, idr, MAX_PAYLOAD); len += MAX_PAYLOAD;
  err = rtp_h264_packetize(au, len, MAX_PAYLOAD, capture_emit, &c);
  fails += !CHECK(err == 0 && c.count == 1 && c.len[0] == MAX_PAYLOAD,
                  "exact fit: err=%d count=%d", err, c.count);

  // No start code at all
  memset(&c, 0, sizeof(c));
  err = rtp_h264_packetize(idr, sizeof(idr), MAX_PAYLOAD, capture_emit, &c);
  fails += !CHECK(err == ERR_MALFORMED && c.count == 0,
                  "no start code: err=%d count=%d", err, c.count);

  fails += run_send();
  return fails;
}

HOSTTEST_SUITE("rtp-h264", run_rtp_h264, 0);
