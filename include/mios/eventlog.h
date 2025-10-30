#pragma once

#ifndef EVENTLOG_SIZE
#define EVENTLOG_SIZE 256
#endif

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


#if EVENTLOG_SIZE

#include <stddef.h>

#include <mios/error.h>

struct stream;

void evlog(event_level_t level, const char *fmt, ...)
  __attribute__ ((format(printf, 2, 3)));

#define evlog_once(gate, level, fmt...) do {    \
  if(!*(gate)) {                                \
    *gate = 1;                                  \
    evlog(level, fmt);                          \
  }                                             \
} while(0)

void eventlog_to_fs(size_t logfile_max_size);

// Special interface to stream directly into eventlog
// Holds global mutex from begin() to end() so use with care
struct stream *evlog_stream_begin(void);

void evlog_stream_end(event_level_t level);

#else

#define evlog(level, fmt...)

#define evlog_once(gate, level, fmt...)

#endif
