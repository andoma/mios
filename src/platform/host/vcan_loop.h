#pragma once

/*
 * Loop a vcan back on itself, with optional faults.
 *
 * Lets several VLLP (or other dsig) endpoints in one host-mios binary
 * talk to each other over a virtual CAN bus: every frame mios transmits
 * is put back into mios's own receive ring unchanged, so a frame sent on
 * signal id N is delivered to whichever endpoint has rxid N. mios never
 * delivers locally emitted dsig to itself, so nothing short-circuits --
 * the frame really does go out and come back.
 *
 * Pair the two ends of a link on opposite ids and each frame lands at
 * exactly one endpoint.
 *
 * Runs as a simulation thread (see cpu/host/sim.h), which is also why the
 * fault injection lives here: it is the one place every frame passes
 * through.
 */

#include <stdint.h>

typedef struct vcan vcan_t;

typedef struct vcan_loop {
  vcan_t *vcan;

  volatile int stop;
  volatile int drop_pct;     /* % of frames discarded */
  volatile int dup_pct;      /* % of frames delivered twice */
  volatile int corrupt_pct;  /* % of frames with one bit flipped */
  volatile int blackhole;    /* discard everything: a link outage */

  /* Counters, so a suite can prove the faults it configured actually
     happened. A fault phase that injected nothing looks exactly like one
     that proved resilience. */
  volatile uint32_t frames;      /* frames seen */
  volatile uint32_t dropped;
  volatile uint32_t duplicated;
  volatile uint32_t corrupted;

  /* One-shot: inject this frame into mios's receive ring next time round,
     i.e. play traffic mios never sent. The only way to reach the code
     that has to reject it. */
  volatile int inject_len;
  uint32_t inject_id;
  uint8_t inject[16];

  uint32_t rng;
} vcan_loop_t;

/* Create the loopback and start its simulation thread. `seed` seeds the
   fault RNG; runs are deterministic for a given seed. */
vcan_loop_t *vcan_loop_create(vcan_t *vcan, uint32_t seed);

/* Discard everything for `us` of virtual time, then let traffic back. A
   link outage long enough to trip the peers' timeouts. Must be called
   from the suite (mios) thread, not the loopback. */
void vcan_loop_outage(vcan_loop_t *lb, uint64_t us);
