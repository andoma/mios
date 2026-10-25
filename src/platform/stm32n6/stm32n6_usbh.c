#include "stm32n6_usb.h"
#include "stm32n6_reg.h"
#include "stm32n6_clk.h"
#include "stm32n6_rif.h"

#include "platform/stm32/stm32_otg_host.c"

#define OTG2_BASE 0x58080000
#define OTG2_IRQ  178

#define OTGPHYC2_BASE 0x580c0000
#define OTGPHYC2_CR   (OTGPHYC2_BASE + 0x00)

#define RIF_MASTER_OTG2      5
#define RIF_PERIPH_OTG2_REG  1
#define RIF_PERIPH_OTG2_BIT  25

void
stm32n6_otghs2_host_create(gpio_t vbus_enable)
{
  rst_assert(CLK_OTG2);
  rst_assert(CLK_OTG2PHY);
  rst_assert(CLK_OTG2PHYCTL);

  clk_enable(CLK_OTG2);
  clk_enable(CLK_OTG2PHY);

  rst_deassert(CLK_OTG2PHYCTL);

  reg_set_bits(OTGPHYC2_CR, 4, 3, 2); // FSEL = 2 (24MHz clock)

  rst_deassert(CLK_OTG2PHY);
  udelay(50);                         // Wait for internal PLL to lock
  rst_deassert(CLK_OTG2);

  // The core is a DMA master, give it the same attributes as ETH1
  stm32n6_rif_set_master_attr(RIF_MASTER_OTG2, 1, 1, 1);
  stm32n6_rif_set_periph_sec(RIF_PERIPH_OTG2_REG, RIF_PERIPH_OTG2_BIT, 1, 1);

  stm32_otg_host_create(OTG2_BASE, OTG2_IRQ, vbus_enable, "usbh");
}
