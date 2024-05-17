#include "nrf52_reg.h"
#include "nrf52_rtc.h"
#include "nrf52_clk.h"

#include "irq.h"

static uint32_t rtc_overflows;


void
exc_systick(void)
{
}


void
irq_36(void)
{
  if(reg_rd(RTC2_BASE + RTC_EVENTS_OVRFLW)) {
    reg_wr(RTC2_BASE + RTC_EVENTS_OVRFLW, 0);
    rtc_overflows++;
  }
}


int64_t
clock_get_irq_blocked(void)
{
  uint32_t counter;

  while(1) {

    counter = reg_rd(RTC2_BASE + RTC_COUNTER);
    if(reg_rd(RTC2_BASE + RTC_EVENTS_OVRFLW)) {
      reg_wr(RTC2_BASE + RTC_EVENTS_OVRFLW, 0);
      rtc_overflows++;
      continue;
    }

    uint64_t n = (uint64_t)counter + ((uint64_t)rtc_overflows << 24);
    return (n * 1000000ULL) >> 15;
  }
}


uint64_t
clock_get(void)
{
  int s = irq_forbid(IRQ_LEVEL_CLOCK);
  uint64_t r = clock_get_irq_blocked();
  irq_permit(s);
  return r;
}


void
udelay(unsigned int usec)
{
  int s = irq_forbid(IRQ_LEVEL_CLOCK);
  uint64_t deadline = clock_get_irq_blocked() + usec;
  while(clock_get_irq_blocked() < deadline) {}
  irq_permit(s);
}


static void  __attribute__((constructor(130)))
nrf52_rtc_init(void)
{
  reg_wr(RTC2_BASE + RTC_TASKS_START, 1);
  reg_wr(RTC2_BASE + RTC_INTENSET, (1 << 1)); // overflow
  irq_enable(36, IRQ_LEVEL_CLOCK);
}
