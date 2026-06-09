#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <sys/queue.h>

#include "eventlog.h"

// Alert severity, independent of the event-log levels. Ordered by
// ascending severity: ATTENTION sits between WARNING and ERROR.
typedef enum {
  ALERT_LEVEL_NOTICE,
  ALERT_LEVEL_WARNING,
  ALERT_LEVEL_ATTENTION,
  ALERT_LEVEL_ERROR,
} alert_level_t;

typedef struct alert_source {
  SLIST_ENTRY(alert_source) as_link;
  const struct alert_class *as_class;
  const char *as_key;
  int as_code;
} alert_source_t;


typedef struct alert_class {
  void (*ac_message)(const struct alert_source *as, struct stream *output);
  alert_level_t (*ac_level)(const struct alert_source *as);
  void (*ac_refcount)(struct alert_source *as, int value);
} alert_class_t;


void alert_register(alert_source_t *as, const alert_class_t *ac,
                    const char *key);

void alert_unregister(alert_source_t *as);

int alert_set(alert_source_t *as, int code);

alert_source_t *alert_get_next(alert_source_t *as);

const char *alert_level_to_string(alert_level_t level);
