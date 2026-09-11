#pragma once

// Radio arbiter: owns the SX1280 and hands out timed execution slots.
//
// Clients submit slots with an earliest start time, a priority and a
// worst-case duration. The scheduler runs one slot at a time on a
// dedicated radio thread; the slot callback owns the radio (issues
// transport commands, blocks on IRQs) until it returns. A slot is not
// started if a higher-priority slot is due to start before it would
// finish, so tightly anchored windows (BLE connection events) are not
// delayed by loose ones (advertising).

#include "sx1280.h"

#include <sys/queue.h>

typedef struct sx1280_slot {
  LIST_ENTRY(sx1280_slot) ss_link;

  // Runs on the radio thread. Returns the absolute time (µs) for the
  // next invocation, or 0 to stop.
  int64_t (*ss_execute)(struct sx1280_slot *ss, sx1280_t *radio,
                        int64_t now);

  int64_t ss_time;      // Earliest start, absolute µs
  uint32_t ss_duration; // Worst-case runtime, µs
  uint8_t ss_prio;      // Higher wins when contending
  uint8_t ss_queued;
} sx1280_slot_t;

void sx1280_sched_submit(sx1280_t *s, sx1280_slot_t *slot, int64_t time);

// Slot start lateness: the maximum since the previous call (reset by the
// call) and the running count of starts more than 20 ms late. Lateness
// on the highest-priority thread is the MCU stalling.
void sx1280_sched_late(sx1280_t *s, uint32_t *max_us, uint32_t *count);

void sx1280_sched_cancel(sx1280_t *s, sx1280_slot_t *slot);

// Modem-config ownership: returns 1 if the mode changed since the
// caller last held it (radio needs full reconfig), 0 if still current.
// NULL invalidates.
int sx1280_sched_set_mode(sx1280_t *s, const void *token);

// Reset the chip and invalidate the mode cache. For clients whose
// slot hit a transport error.
error_t sx1280_sched_recover(sx1280_t *s);
