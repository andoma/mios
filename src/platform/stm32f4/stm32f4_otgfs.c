#include "stm32f4_usb.h"
#include "stm32f4_reg.h"
#include "stm32f4_clk.h"

#define MAX_NUM_ENDPOINTS 6
#define OTG_BASE 0x50000000

#include "platform/stm32/stm32_otg.c"

void
stm32f4_otgfs_create(uint16_t vid, uint16_t pid,
                     const char *manfacturer_string,
                     const char *product_string,
                     struct usb_interface_queue *q)
{
  gpio_conf_af(GPIO_PA(11), 10, GPIO_OPEN_DRAIN,
               GPIO_SPEED_VERY_HIGH, GPIO_PULL_NONE);
  gpio_conf_af(GPIO_PA(12), 10, GPIO_OPEN_DRAIN,
               GPIO_SPEED_VERY_HIGH, GPIO_PULL_NONE);

  clk_enable(CLK_OTG);
  reset_peripheral(RST_OTG);

  stm32_otg_create(vid, pid, manfacturer_string, product_string, q, 67);
}
