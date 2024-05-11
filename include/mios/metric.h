#pragma once

#include <sys/queue.h>

#include <stdint.h>

SLIST_HEAD(metric_slist, metric);

extern struct metric_slist metrics;

typedef struct metric_def {
  const char *name;

  float hi_error;    // Set to NAN if not applicable
  float hi_warning;  // Set to NAN if not applicable
  float lo_warning;  // Set to NAN if not applicable
  float lo_error;    // Set to NAN if not applicable
  float hysteresis;  // error/warning trip hysersis

  char unit;              /* ASCII char representing unit (V=voltage, etc)
                             No significance other than for presentation */

  uint8_t alert_bit;      // Will raise alert if value is outside threshold

  uint8_t emit_period;    // Seconds

  uint8_t irq_level;      /* IRQ that needs to be forbidden for atomic
                           * readout of stats variables.
                           * Set to same IRQ level as the IRQ-handler
                           * that calls metric_acc()
                           *
                           * If metric is only modified on thread context
                           * IRQ_LEVEL_SWITCH is the correct choice.
                           */

  uint8_t alert_lockout_duration; /* Seconds before we start send
                                     alerts after metric_reset() */

} metric_def_t;


typedef struct metric {
  SLIST_ENTRY(metric) link;
  const metric_def_t *def;
  float min;
  float max;
  float mean;
  float m2;
  unsigned int count;
  uint8_t update_counter;
  uint8_t state;
  uint8_t raised_alerts;
  uint8_t alert_lockout;
} metric_t;

#define METRIC_HI_ERROR   0x1
#define METRIC_HI_WARNING 0x2
#define METRIC_LO_WARNING 0x4
#define METRIC_LO_ERROR   0x8

#define METRIC_STATE_OFF     0
#define METRIC_STATE_ON      1
#define METRIC_STATE_FREEZED 2

void metric_init(metric_t *m, const metric_def_t *def, uint8_t state);

void metric_reset(metric_t *m, int enable);

// irq_forbid(m->def->irq_level) is assumed
void metric_collect(metric_t *m, float v);

// Returns 1 if fault state changed
int metric_update_fault(metric_t *m, float v);
