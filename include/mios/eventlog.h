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

void evlog(event_level_t level, const char *fmt, ...)
  __attribute__ ((format(printf, 2, 3)));


void eventlog_to_fs(size_t logfile_max_size);

#else

#define evlog(level, fmt...)

#define evlogst(level, st, fmt...)

#endif
