#pragma once

#ifndef EVENTLOG_SIZE
#define EVENTLOG_SIZE 256
#endif

#if EVENTLOG_SIZE

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

void evlog0(event_level_t level, const char *fmt, ...);

#define evlog(level, fmt...) evlog0(level, fmt)

#else

#define evlog(level, fmt...)

#endif
