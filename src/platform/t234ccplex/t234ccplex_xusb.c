#include "reg.h"



#define XUSB_PADCTL_BASE 0x3520000

#define XUSB_PADCTL_USB2_PORT_CAP_0                     0x8

static void __attribute__((constructor(300)))
xusb_init(void)
{
  // Turn off USB2 ports. This makes us disconnect from host in RCMBOOT
  reg_wr(XUSB_PADCTL_BASE + XUSB_PADCTL_USB2_PORT_CAP_0, 0);
}
