#pragma once

#include <mios/io.h>

// gpio_t encodes (port << 5) | pin.  nRF54L has three GPIO ports, each a
// separate peripheral instance (secure base addresses).
#define GPIO_PORT_BASE(g)                       \
  ((g) < GPIO_P1(0) ? 0x5010a000 :   /* P0 */   \
   (g) < GPIO_P2(0) ? 0x500d8200 :   /* P1 */   \
                      0x50050400)    /* P2 */

#define GPIO_PIN(g)        ((g) & 0x1f)

#define GPIO_OUTSET(g)     (GPIO_PORT_BASE(g) + 0x004)
#define GPIO_OUTCLR(g)     (GPIO_PORT_BASE(g) + 0x008)
#define GPIO_IN(g)         (GPIO_PORT_BASE(g) + 0x00c)
#define GPIO_PIN_CNF(g)    (GPIO_PORT_BASE(g) + 0x080 + GPIO_PIN(g) * 4)
