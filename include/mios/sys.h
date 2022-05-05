#pragma once

#include "io.h"

typedef enum {
  RESET_REASON_UNKNOWN         = 0x0,
  RESET_REASON_LOW_POWER_RESET = 0x1,
  RESET_REASON_WATCHDOG        = 0x2,
  RESET_REASON_SW_RESET        = 0x3,
  RESET_REASON_POWER_ON        = 0x4,
  RESET_REASON_EXT_RESET       = 0x5,
  RESET_REASON_BROWNOUT        = 0x6,
  RESET_REASON_OTHER           = 0x7
} reset_reason_t;

reset_reason_t sys_get_reset_reason(void);

struct serial_number {
  const void *data;
  size_t len;
};

const struct serial_number sys_get_serial_number(void);

void sys_watchdog_start(gpio_t blink);
