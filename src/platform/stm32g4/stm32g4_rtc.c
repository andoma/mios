#include "stm32g4_rtc.h"

#include "stm32g4_reg.h"
#include "stm32g4_clk.h"

#include <mios/datetime.h>

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

  reg_wr(RTC_WPR, 0xCA);
  reg_wr(RTC_WPR, 0x53);
}

void
stm32g4_rtc_set(void)
{
  stm32g4_rtc_enable();

  datetime_t dt;
  datetime_from_unixtime(datetime_get_utc_sec(), &dt);

  uint32_t su = dt.sec % 10;
  uint32_t st = dt.sec / 10;
  uint32_t mnu = dt.min % 10;
  uint32_t mnt = dt.min / 10;
  uint32_t hu = dt.hour % 10;
  uint32_t ht = dt.hour / 10;

  uint32_t mdu = dt.mday % 10;
  uint32_t mdt = dt.mday / 10;

  uint32_t mu = dt.mon % 10;
  uint32_t mt = dt.mon / 10;

  uint32_t wdu = datetime_day_of_week(&dt);

  uint32_t y = dt.year % 100;
  uint32_t yu = y % 10;
  uint32_t yt = y / 10;

  uint32_t tr =
    (su << 0) |
    (st << 4) |
    (mnu << 8) |
    (mnt << 12) |
    (hu << 16) |
    (ht << 20);

  uint32_t dr =
    (mdu << 0) |
    (mdt << 4) |
    (mu << 8) |
    (mt << 12) |
    (wdu << 13) |
    (yu << 16) |
    (yt << 20);

  reg_set_bit(RTC_ICSR, 7);
  while(reg_get_bit(RTC_ICSR, 6) == 0) {}
  reg_wr(RTC_TR, tr);
  reg_wr(RTC_DR, dr);
  reg_clr_bit(RTC_ICSR, 7);
}

#if 0
#include <mios/cli.h>

static error_t
cmd_setrtc(cli_t *cli, int argc, char **argv)
{
  stm32g4_rtc_set();
  return 0;
}
CLI_CMD_DEF("setrtc", cmd_setrtc);


static error_t
cmd_getrtc(cli_t *cli, int argc, char **argv)
{
  stm32g4_rtc_enable();

  if(!reg_get_bit(RTC_ICSR, 5))
    return ERR_NOT_READY;

  printf("TR: 0x%08x\n", reg_rd(RTC_TR));
  printf("DR: 0x%08x\n", reg_rd(RTC_DR));
  return 0;
}
CLI_CMD_DEF("getrtc", cmd_getrtc);
#endif
