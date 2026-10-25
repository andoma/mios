#include "stm32n6_ucpd.h"
#include "stm32n6_reg.h"
#include "stm32n6_clk.h"

#include "platform/stm32/stm32_ucpd.c"

#define UCPD1_BASE 0x5000fc00
#define UCPD1_IRQ  176

#define RCC_APB1HENR (RCC_BASE + 0x268)
#define CLK_UCPD1    CLK_ID(RCC_APB1HENR, 18)

typec_phy_t *
stm32n6_ucpd_create(void)
{
  clk_enable(CLK_UCPD1);
  rst_assert(CLK_UCPD1);
  rst_deassert(CLK_UCPD1);

  // The CC pins are dedicated, and the kernel clock is fixed at HSI / 4
  return stm32_ucpd_create(UCPD1_BASE, UCPD1_IRQ, 64000000 / 4);
}
