#pragma once

#include <stdint.h>

typedef struct wallclock {
  int64_t utc_offset;  // In µS. Add this to clock_get() for current time
  int tz_offset;       // In seconds
  const char *source;
} wallclock_t;

extern wallclock_t wallclock;

typedef struct datetime {
  uint16_t year;
  uint8_t mon;
  uint8_t mday;
  uint8_t hour;
  uint8_t min;
  uint8_t sec;
} datetime_t;

void datetime_from_unixtime(uint32_t t, datetime_t *dt);

int datetime_day_of_week(const datetime_t *dt); // 1=Monday 7=Sunday (ISO)

uint64_t datetime_get_utc_usec(void);

uint32_t datetime_get_utc_sec(void);

void datetime_set_utc_offset(int64_t offset, const char *source);

typedef enum {
  DATETIME_YEAR,
  DATETIME_MON,
  DATETIME_MDAY,
  DATETIME_HOUR,
  DATETIME_MIN,
  DATETIME_SEC,
} datetime_adj_hand_t;

void datetime_adj(datetime_adj_hand_t which, int delta);

