#include "stm32g4_rtc.h"

#include "stm32g4_reg.h"
#include "stm32g4_clk.h"

void
stm32g4_rtc_enable(void)
{
  if(clk_is_enabled(CLK_RTC))
    return;

  stm32g4_enable_backup_domain();

  int rtc_clock = 0b10;  // LSI
  if(reg_get_bit(RCC_BDCR, 1)) {
    rtc_clock = 0b01; // LSE
  }

  reg_set_bits(RCC_BDCR, 8, 2, rtc_clock);
  reg_set_bit(RCC_BDCR, 15);

  clk_enable(CLK_RTC);
  reg_wr(RTC_SCR, -1);
}
