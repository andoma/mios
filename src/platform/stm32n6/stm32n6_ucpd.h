#pragma once

#include <usb/typec.h>

// UCPD1, the Type-C PHY for the port wired to OTG1
typec_phy_t *stm32n6_ucpd_create(void);
