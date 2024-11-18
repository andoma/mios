#pragma once

#include <stdint.h>

typedef enum {
  POLL_NONE,
  POLL_COND,
  POLL_STREAM_READ,
  POLL_STREAM_WRITE,
} poll_type_t;

typedef struct pollset {
  void *obj;
  poll_type_t type;
} pollset_t;

struct mutex;

/**
 * @ps is an array of objects to wait for.
 * @num number of elemsnts in @ps array
 *
 * @m interlock mutex (unlocked similar to how cond_wait does it,
 * this is a necessity when waiting for condition variable for correct
 * interloc). Can be NULL if no interlock is needed
 *
 * @deadline if no events was raised before this, give up.
 * INT64_MAX means to wait indefinitely
 *
 * poll returns index of first object that woke up (there's currently
 * no support really for multiple objects signalling wakeup status)
 *
 * if timeout expired -1 will be returned
 *
 */
int poll(const pollset_t *ps, size_t num, struct mutex *m, int64_t deadline);
