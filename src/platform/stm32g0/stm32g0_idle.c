#include "irq.h"

#include <mios/device.h>

#include "stm32g0_reg.h"
#include "stm32g0_clk.h"
#include "stm32g0_pwr.h"
#include "stm32g0_rtc.h"
#include "stm32g0_wdog.h"

#define WDOG_HZ 128
#define RTC_HZ 2048

#define WDOG_TIMEOUT_SEC 30

static volatile unsigned int * const SCR  = (unsigned int *)0xe000ed10;

static int wakelock = 0x10000000;


void
wakelock_acquire(void)
{
  int q = irq_forbid(IRQ_LEVEL_ALL);
  wakelock++;
  irq_permit(q);
}

void
wakelock_release(void)
{
  int q = irq_forbid(IRQ_LEVEL_ALL);
  wakelock--;
  irq_permit(q);
}

static void
wdog_init(void)
{
  reg_wr(IWDG_KR, 0x5555);
  reg_wr(IWDG_RLR, WDOG_HZ * WDOG_TIMEOUT_SEC);
  reg_wr(IWDG_PR, 6);  // Prescaler (/256) for 32768 -> WDOG_HZ
  reg_wr(IWDG_KR, 0xAAAA);
  reg_wr(IWDG_KR, 0xCCCC);
}


void  __attribute__((noreturn))
cpu_idle(void)
{
  wdog_init();

  while(1) {
    if(!wakelock) {

      asm volatile ("cpsid i");
      *SCR = 0x4;
      device_power_state(DEVICE_POWER_STATE_SUSPEND);
      asm("wfi");
      *SCR = 0x0;
      stm32g0_init_pll();
      device_power_state(DEVICE_POWER_STATE_RESUME);
      asm volatile ("cpsie i");
    } else {
      asm("wfi");
    }
    reg_wr(IWDG_KR, 0xAAAA);
  }
}


void
irq_2(void)
{
  uint32_t sr = reg_rd(RTC_SR);
  reg_wr(RTC_SCR, sr);
  if(!wakelock)
    reg_wr(IWDG_KR, 0xAAAA);
}



static void
enable_rtc_wakeups(void)
{
  reg_set_bit(PWR_CR1, 8);

  reg_wr(RCC_BDCR,
         (1 << 15) |
         (2 << 8));

  clk_enable(CLK_RTC);
  reg_wr(RTC_SCR, -1);
  irq_enable(2, IRQ_LEVEL_CLOCK);

  reg_wr(RTC_WPR, 0xCA);
  reg_wr(RTC_WPR, 0x53);

  reg_clr_bit(RTC_CR, 10);

  reg_wr(RTC_CR, 0);

  //wait for WUTWF is set
  while(!(reg_rd(RTC_ICSR) & 0x4)) {}
  // write WUT
  reg_wr(RTC_WUTR, RTC_HZ * (WDOG_TIMEOUT_SEC - 1));

  reg_wr(RTC_CR,
         (1 << 14) |
         (1 << 10));
}

void
suspend_enable(void)
{
  int q = irq_forbid(IRQ_LEVEL_ALL);
  if(wakelock & 0x10000000) {
    clk_enable(CLK_PWR);
    enable_rtc_wakeups();
    reg_set_bits(PWR_CR1, 0, 3, 1); // Stop mode 1 (Low power)
    wakelock &= ~0x10000000;
  }
  irq_permit(q);
}
