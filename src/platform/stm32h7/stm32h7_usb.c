#include "stm32h7_usb.h"
#include "stm32h7_reg.h"
#include "stm32h7_clk.h"
#include "stm32h7_pwr.h"
#include "stm32h7_csr.h"

#define MAX_NUM_ENDPOINTS 9
#define OTG_BASE 0x40040000

#include "platform/stm32/stm32_otg.c"

void
stm32h7_otghs_create(uint16_t vid, uint16_t pid,
                     const char *manfacturer_string,
                     const char *product_string,
                     struct usb_interface_queue *q)
{
  // Clear USB1OTGHSULPILPEN
  // Otherwise the USB PHY will when CPU is sleeping with "wfi"
  reg_clr_bit(RCC_AHB1LPENR, 26);

  // USB3v3 regulator
  reg_set_bit(PWR_CR3, 24);
  while(reg_get_bit(PWR_CR3, 26) == 0) {}

  clk_enable(CLK_CSR);
  reset_peripheral(RST_CSR);

  reg_or(CRS_CR, 0x60); // Clock recovery from USB

  clk_enable(CLK_OTG);
  reset_peripheral(RST_OTG);

  stm32_otg_create(vid, pid, manfacturer_string, product_string, q, 77);
}
