/*
 * Link simulator / fault injector.
 *
 * Two independent directions (0 and 1), each with its own fault config
 * and counters. Frames go through a delivery thread so delay and
 * reordering are possible; with no faults configured frames are
 * delivered in order as fast as the thread runs.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

typedef struct fault_cfg {
  double drop;          /* probability a frame is dropped            */
  double dup;           /* probability a frame is delivered twice    */
  double corrupt;       /* probability one random bit is flipped     */
  int delay_min_us;     /* per-frame delay uniformly in [min, max]   */
  int delay_max_us;     /* max > min allows reordering               */
  int blackout;         /* drop everything while set                 */
} fault_cfg_t;

typedef struct linksim_stats {
  uint64_t frames;
  uint64_t bytes;
  uint64_t dropped;
  uint64_t dupped;
  uint64_t corrupted;
  uint64_t delivered;
} linksim_stats_t;

typedef struct linksim linksim_t;

typedef void (*linksim_deliver_fn)(void *opaque, const void *data, size_t len);

linksim_t *linksim_create(uint64_t seed);

void linksim_destroy(linksim_t *ls);

void linksim_set_faults(linksim_t *ls, int dir, const fault_cfg_t *cfg);

void linksim_get_faults(linksim_t *ls, int dir, fault_cfg_t *cfg);

void linksim_send(linksim_t *ls, int dir, const void *data, size_t len,
                  linksim_deliver_fn fn, void *opaque);

void linksim_get_stats(linksim_t *ls, int dir, linksim_stats_t *st);

void linksim_reset_stats(linksim_t *ls);

/* Decoded per-frame trace (docs/vllp.txt) to fp, NULL disables. fdcan:
 * strip the FDCAN padding byte before decoding (MTU > 8 sessions). */
void linksim_set_trace(linksim_t *ls, void *fp, int fdcan);

/* Wait until nothing is queued in either direction (or timeout) */
int linksim_drain(linksim_t *ls, int64_t timeout_us);
