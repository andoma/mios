/*
 * pbuf: helpers that walk chains of packet buffers
 */

#include <string.h>

#include <net/pbuf.h>
#include <net/ipv4/ipv4.h>

#include "hosttest.h"

// Three buffers holding bytes 0..259 of a counting pattern, each with a
// start offset. A buffer allocated in between keeps the data of the
// first two from being adjacent in memory, so reading past the end of
// one doesn't happen to land on the next
static pbuf_t *
make_chain_of(pbuf_t **spacer, const int *seglen)
{
  pbuf_t *head = NULL, *prev = NULL;
  int v = 0;
  for(int i = 0; i < 3; i++) {
    pbuf_t *pb = pbuf_make(10 + i * 10, 1);
    uint8_t *p = pbuf_append(pb, seglen[i]);
    for(int j = 0; j < seglen[i]; j++)
      p[j] = v++;
    pb->pb_flags = i == 0 ? PBUF_SOP : i == 2 ? PBUF_EOP : 0;
    if(prev)
      prev->pb_next = pb;
    else
      head = pb;
    prev = pb;
    if(i == 0)
      *spacer = pbuf_make(0, 1);
  }
  head->pb_pktlen = 260;
  return head;
}


static pbuf_t *
make_chain(pbuf_t **spacer)
{
  static const int seglen[3] = { 100, 80, 80 };
  return make_chain_of(spacer, seglen);
}


// The Internet checksum of bytes [offset, offset + len) of the counting
// pattern, the plain way
static uint16_t
cksum_ref(size_t offset, size_t len)
{
  uint32_t sum = 0;
  for(size_t i = 0; i < len; i++)
    sum += (uint8_t)(offset + i) << (i & 1 ? 0 : 8);
  while(sum >> 16)
    sum = (sum & 0xffff) + (sum >> 16);
  // As it is stored in a packet, most significant byte first
  const uint8_t be[2] = { ~sum >> 8, ~sum };
  uint16_t r;
  memcpy(&r, be, 2);
  return r;
}

static int
cksum_ok(pbuf_t *pb, size_t offset, size_t len)
{
  return ipv4_cksum_pbuf(0, pb, offset, len) == cksum_ref(offset, len);
}


static int
read_ok(pbuf_t *pb, size_t offset, size_t len)
{
  uint8_t buf[260];
  memset(buf, 0xee, sizeof(buf));
  if(pbuf_read_at(pb, buf, offset, len))
    return 0;
  for(size_t i = 0; i < len; i++) {
    if(buf[i] != (uint8_t)(offset + i))
      return 0;
  }
  return 1;
}


static int
run_pbuf(void)
{
  pbuf_t *spacer;
  pbuf_t *pb = make_chain(&spacer);

  CHECK(read_ok(pb, 0, 10), "start of first buffer");
  CHECK(read_ok(pb, 20, 50), "within first buffer");
  CHECK(read_ok(pb, 90, 20), "across first and second, from an offset");
  CHECK(read_ok(pb, 28, 200), "across all three, from an offset");
  CHECK(read_ok(pb, 120, 100), "across second and third");
  CHECK(read_ok(pb, 0, 260), "everything");
  CHECK(read_ok(pb, 259, 1), "last byte");
  uint8_t b[4];
  CHECK(pbuf_read_at(pb, b, 258, 4) == -1, "past the end must fail");
  CHECK(pbuf_read_at(pb, b, 300, 1) == -1, "offset past the end must fail");

  uint8_t pattern[200];
  for(int i = 0; i < 200; i++)
    pattern[i] = 28 + i;
  CHECK(pbuf_memcmp_at(pb, pattern, 28, 200) == 0,
        "memcmp across all three, from an offset");
  pattern[150] ^= 1;
  CHECK(pbuf_memcmp_at(pb, pattern, 28, 200) != 0,
        "memcmp must see a difference in the third buffer");
  for(int i = 0; i < 200; i++)
    pattern[i] = 100 + i; // Matches up to the end of the chain
  CHECK(pbuf_memcmp_at(pb, pattern, 100, 200) == -1,
        "memcmp past the end must fail");

  CHECK(cksum_ok(pb, 0, 260), "checksum of everything");
  CHECK(cksum_ok(pb, 20, 201), "checksum across all three, odd length");
  CHECK(cksum_ok(pb, 100, 80), "checksum of the second buffer");

  pbuf_free(pb);
  pbuf_free(spacer);

  // Buffers of odd length: 16 bit words that straddle two of them
  static const int odd[3] = { 99, 81, 80 };
  pb = make_chain_of(&spacer, odd);
  CHECK(cksum_ok(pb, 0, 260), "odd buffers: checksum of everything");
  CHECK(cksum_ok(pb, 0, 99), "odd buffers: the first one");
  CHECK(cksum_ok(pb, 21, 200), "odd buffers: from an odd offset");
  CHECK(cksum_ok(pb, 98, 3), "odd buffers: across the first boundary");
  CHECK(cksum_ok(pb, 99, 161), "odd buffers: from the second on");
  CHECK(cksum_ok(pb, 50, 209), "odd buffers: odd length to the last byte");
  pbuf_free(pb);
  pbuf_free(spacer);

  return 0; // failures are counted by CHECK()
}

HOSTTEST_SUITE("pbuf", run_pbuf, 0);
