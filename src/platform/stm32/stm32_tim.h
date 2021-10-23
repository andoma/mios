#pragma once

typedef struct {
  uint32_t base;
  uint16_t clk;
  uint8_t irq;
} stm32_timer_info_t;

//const stm32_timer_info_t *stm32_get_timer(int id);
