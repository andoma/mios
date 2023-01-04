#pragma once

#ifndef EVENTLOG_SIZE
#define EVENTLOG_SIZE 256
#endif

#if EVENTLOG_SIZE

#include <stddef.h>

#include <mios/error.h>

typedef enum {
  LOG_EMERG,
  LOG_ALERT,
  LOG_CRIT,
  LOG_ERR,
  LOG_WARNING,
  LOG_NOTICE,
  LOG_INFO,
  LOG_DEBUG,
} event_level_t;

struct stream;

void evlog0(event_level_t level, struct stream *st, const char *fmt, ...)
  __attribute__ ((format(printf, 3, 4)));

#define evlog(level, fmt...) evlog0(level, NULL, fmt)

#define evlogst(level, st, fmt...) evlog0(level, st, fmt)

struct pcs;

error_t evlog_to_pcs(struct pcs *pcs);

#else

#define evlog(level, fmt...)

#define evlogst(level, st, fmt...)

#endif
