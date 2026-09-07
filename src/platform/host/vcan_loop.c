#include <stdlib.h>
#include <unistd.h>

#include <mios/mios.h>

#include "sim.h"
#include "vcan.h"
#include "vcan_loop.h"

#define VCAN_LOOP_MAX_FRAME 80


static uint32_t
rng32(vcan_loop_t *lb)
{
  uint32_t x = lb->rng ? lb->rng : 0x2545f491;
  x ^= x << 13; x ^= x >> 17; x ^= x << 5;
  lb->rng = x;
  return x;
}


static int
roll(vcan_loop_t *lb, int pct)
{
  return pct > 0 && (rng32(lb) % 100) < (uint32_t)pct;
}


static void
vcan_loop_fn(void *arg)
{
  vcan_loop_t *lb = arg;
  uint8_t frame[VCAN_LOOP_MAX_FRAME];
  uint32_t id;

  while(!lb->stop) {

    if(lb->inject_len > 0) {
      vcan_peer_send(lb->vcan, lb->inject_id, (const void *)lb->inject,
                     lb->inject_len);
      lb->inject_len = 0;
    }

    // A long deadline rather than SIM_NEVER: the loop has to come back
    // round now and then to notice `stop` and pending injections.
    long n = vcan_peer_recv(lb->vcan, &id, frame, sizeof(frame),
                            clock_get() + 10000000);
    if(n < 0)
      continue;

    lb->frames++;

    if(lb->blackhole || roll(lb, lb->drop_pct)) {
      lb->dropped++;
      continue;
    }

    if(roll(lb, lb->corrupt_pct) && n > 0) {
      frame[rng32(lb) % n] ^= 1 << (rng32(lb) & 7);
      lb->corrupted++;
    }

    vcan_peer_send(lb->vcan, id, frame, n);

    if(roll(lb, lb->dup_pct)) {
      vcan_peer_send(lb->vcan, id, frame, n);
      lb->duplicated++;
    }
  }
}


vcan_loop_t *
vcan_loop_create(vcan_t *vcan, uint32_t seed)
{
  vcan_loop_t *lb = calloc(1, sizeof(vcan_loop_t));
  lb->vcan = vcan;
  lb->rng = seed ? seed : 0x2545f491;
  sim_thread_create("vcan-loop", vcan_loop_fn, lb, 1 << 18);
  return lb;
}


void
vcan_loop_outage(vcan_loop_t *lb, uint64_t us)
{
  lb->blackhole = 1;
  const uint64_t deadline = clock_get() + us;
  while(clock_get() < deadline)
    usleep(100000);
  lb->blackhole = 0;
}
