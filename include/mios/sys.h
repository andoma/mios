#pragma once

#include "io.h"

typedef enum {
  RESET_REASON_LOW_POWER_RESET = (1 << 0),
  RESET_REASON_WATCHDOG        = (1 << 1),
  RESET_REASON_SW_RESET        = (1 << 2),
  RESET_REASON_POWER_ON        = (1 << 3),
  RESET_REASON_EXT_RESET       = (1 << 4),
  RESET_REASON_BROWNOUT        = (1 << 5),
  RESET_REASON_CPU_LOCKUP      = (1 << 6),
  RESET_REASON_GPIO            = (1 << 7),
  RESET_REASON_COMPARATOR      = (1 << 8),
  RESET_REASON_DEBUG           = (1 << 9),
  RESET_REASON_NFC             = (1 << 10),
} reset_reason_t;

reset_reason_t sys_get_reset_reason(void);

struct serial_number {
  const void *data;
  size_t len;
};

const struct serial_number sys_get_serial_number(void);

void sys_watchdog_start(gpio_t blink);

extern const char *reset_reasons;
