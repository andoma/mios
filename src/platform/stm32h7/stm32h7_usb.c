#include "stm32h7_usb.h"
#include "stm32h7_reg.h"
#include "stm32h7_clk.h"
#include "stm32h7_pwr.h"
#include "stm32h7_csr.h"

#define MAX_NUM_ENDPOINTS 9
#define OTG_BASE 0x40040000

#include "platform/stm32/stm32_otg.c"

static inline void
otg_platform_init_regs(usb_ctrl_t *uc)
{
  reg_set_bits(OTG_GUSBCFG, 10, 4, 6); // turnaround time
  reg_set_bit(OTG_GUSBCFG, 6);         // Select internal PHY
  reg_set_bit(OTG_GCCFG, 21);          // Disable VBUS mon
  reg_wr(OTG_DCFG, (reg_rd(OTG_DCFG) & 0xffff0000) | 3); // Speed = FS (Int)
  reg_set_bits(OTG_GCCFG, 16, 1, 1);   // Power up
}


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
  reset_peripheral(CLK_CSR);

  reg_or(CRS_CR, 0x60); // Clock recovery from USB

  clk_enable(CLK_OTG);
  reset_peripheral(CLK_OTG);

  usb_ctrl_t *uc =
    stm32_otg_create(vid, pid, manfacturer_string, product_string, q, 77);

  const struct serial_number sn = sys_get_serial_number();
  const uint32_t *sn_u32 = sn.data;
  uint32_t sum = sn_u32[0] + sn_u32[2];
  uc->uc_serial_number[0] = sum >> 24;
  uc->uc_serial_number[1] = sum >> 16;
  uc->uc_serial_number[2] = sum >> 8;
  uc->uc_serial_number[3] = sum;
  uc->uc_serial_number[4] = sn_u32[1] >> 24;
  uc->uc_serial_number[5] = sn_u32[1] >> 16;
  uc->uc_serial_number_bytes = 6;

}

int
stm32h7_otghs_is_connected(void)
{
  return !(reg_rd(OTG_DSTS) & 1);
}
