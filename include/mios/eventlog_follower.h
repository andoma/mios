#pragma once

#if EVENTLOG_SIZE

#define EVENTLOG_MASK (EVENTLOG_SIZE - 1)

LIST_HEAD(evlog_follower_list, evlog_follower);

typedef struct {
  int64_t ts_tail;
  int64_t ts_head;
  mutex_t mutex;
  struct evlog_follower_list followers;
  uint16_t head;
  uint16_t tail;
  uint32_t seq_tail;
  uint8_t data[EVENTLOG_SIZE];
} evlog_fifo_t;

typedef struct evlog_follower {
  LIST_ENTRY(evlog_follower) link;
  void (*cb)(struct evlog_follower *f);
  uint16_t ptr;
  uint16_t drops;
} evlog_follower_t;

void evfifo_follower_register(evlog_follower_t *f);

void evfifo_follower_unregister(evlog_follower_t *f);

int64_t evfifo_read_delta_ts(evlog_fifo_t *ef, uint16_t ptr);

evlog_fifo_t *evfifo_get(void);

#endif
