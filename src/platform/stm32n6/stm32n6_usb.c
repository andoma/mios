#include "stm32n6_usb.h"
#include "stm32n6_reg.h"
#include "stm32n6_clk.h"
#include "stm32n6_pwr.h"

#define MAX_NUM_ENDPOINTS 9
#define OTG_BASE 0x58040000

#include "platform/stm32/stm32_otg.c"

#define OTGPHYC1_BASE 0x5803fc00
#define OTGPHYC1_CR   (OTGPHYC1_BASE + 0x00)

static inline void
otg_platform_init_regs(usb_ctrl_t *uc)
{
  reg_set_bits(OTG_GUSBCFG, 10, 4, 9); // turnaround time
  reg_set_bit(OTG_GCCFG, 23);
  reg_wr(OTG_DCFG, (reg_rd(OTG_DCFG) & 0xffff0000) | 1);
}

void
stm32n6_otghs_create(uint16_t vid, uint16_t pid,
                     const char *manfacturer_string,
                     const char *product_string,
                     struct usb_interface_queue *q)
{
  rst_assert(CLK_OTG1);
  rst_assert(CLK_OTG1PHY);
  rst_assert(CLK_OTG1PHYCTL);

  clk_enable(CLK_OTG1);
  clk_enable(CLK_OTG1PHY);

  rst_deassert(CLK_OTG1PHYCTL);

  reg_set_bits(OTGPHYC1_CR, 4, 3, 2); // FSEL = 2 (24MHz clock)

  rst_deassert(CLK_OTG1PHY);
  udelay(50);                         // Wait for internal PLL to lock
  rst_deassert(CLK_OTG1);

  usb_ctrl_t *uc =
    stm32_otg_create(vid, pid, manfacturer_string, product_string, q, 177);

  // Construct serial number the same way as as STM32N6 Bootrom

  const struct serial_number sn = sys_get_serial_number();
  const uint8_t *src = sn.data;
  for(size_t i = 0; i < sizeof(uc->uc_serial_number); i++) {
    uc->uc_serial_number[i] = src[(i & ~3) + (3 - (i & 3))];
  }
  uc->uc_serial_number_bytes = sizeof(uc->uc_serial_number);
}
