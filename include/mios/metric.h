#pragma once

#include <sys/queue.h>

#include <stdint.h>

SLIST_HEAD(metric_slist, metric);

extern struct metric_slist metrics;

typedef struct metric_def {
  const char *name;

  float high_alert;   // Set to NAN if not applicable
  float high_warning; // Set to NAN if not applicable
  float low_warning;  // Set to NAN if not applicable
  float low_alert;    // Set to NAN if not applicable

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
} metric_def_t;


typedef struct metric {
  SLIST_ENTRY(metric) link;
  const metric_def_t *def;
  float min;
  float max;
  float sum;
  float sumsum;
  unsigned int count;
  uint8_t update_counter;
  uint8_t enabled;
} metric_t;

void metric_init(metric_t *m, const metric_def_t *def, uint8_t enabled);

void metric_reset(metric_t *m, int enable);

// irq_forbid(m->def->irq_level) is assumed
void metric_collect(metric_t *m, float v);
