#include "stm32h7_rtc.h"
#include "stm32h7_clk.h"
#include "stm32h7_pwr.h"
#include "stm32h7_reg.h"

#define RTC_BASE   0x58004000
#define RTC_TAFCR  (RTC_BASE + 0x40)

#define PWR_CR1    (PWR_BASE + 0x00)
#define PWR_CR1_DBP 8

void
stm32h7_rtc_force_pin(int pin, int value)
{
  if(pin < 13 || pin > 15)
    return;
  // Backup domain writes are locked until DBP is set; RTC_TAFCR is not
  // behind the RTC write-protection keys, only behind DBP.
  reg_set_bit(PWR_CR1, PWR_CR1_DBP);
  clk_enable(CLK_RTCAPB);
  const int mode_bit = 19 + 2 * (pin - 13);
  const int value_bit = 18 + 2 * (pin - 13);
  uint32_t v = reg_rd(RTC_TAFCR);
  v |= 1u << mode_bit;
  if(value)
    v |= 1u << value_bit;
  else
    v &= ~(1u << value_bit);
  reg_wr(RTC_TAFCR, v);
}
