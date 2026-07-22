#include "nrf52_clk.h"
#include "nrf52_reg.h"

#include "irq.h"

// The LFCLK runs from the internal RC oscillator (the nRF52840 Dongle has no
// 32 kHz crystal). It needs periodic calibration against the HFCLK to stay
// accurate. Who does that depends on the build:
//
//  - No BLE: mios owns the CLOCK peripheral and calibrates it here (irq_0).
//  - With BLE: MPSL owns the CLOCK peripheral (POWER_CLOCK IRQ) and calibrates
//    the RC for the radio; our RTCs ride the same LFCLK. mios must not touch
//    the CLOCK IRQ then, only start the LFCLK early so the RTCs tick from boot
//    (MPSL comes up much later, in the SDC constructor).

#ifndef ENABLE_NET_BLE

static uint8_t xtal_want;
static uint8_t xtal_have;

#define XTAL_CALIB 0x1


static void
xtal_clear(uint8_t flag)
{
  xtal_want &= ~flag;
  xtal_have &= ~flag;

  if(!xtal_have) {
    reg_wr(CLOCK_TASKS_HFCLKSTOP, 1);
  }
}

static void
xtal_got(void)
{
  uint8_t got = (xtal_have ^ xtal_want) & xtal_want;
  xtal_have |= xtal_want;

  if(got & XTAL_CALIB)
    reg_wr(CLOCK_TASKS_CAL, 1);
}


static void
xtal_set(uint8_t flag)
{
  xtal_want |= flag;

  if(!xtal_have) {
    reg_wr(CLOCK_TASKS_HFCLKSTART, 1);
  } else {
    xtal_got();
  }
}


void
irq_0(void)
{
  if(reg_rd(CLOCK_EVENTS_CTTO)) {
    reg_wr(CLOCK_EVENTS_CTTO, 0);
    xtal_set(XTAL_CALIB);
  }

  if(reg_rd(CLOCK_EVENTS_HFCLKSTARTED)) {
    reg_wr(CLOCK_EVENTS_HFCLKSTARTED, 0);
    xtal_got();
  }

  if(reg_rd(CLOCK_EVENTS_DONE)) {
    reg_wr(CLOCK_EVENTS_DONE, 0);

    xtal_clear(XTAL_CALIB);
    reg_wr(CLOCK_TASKS_CTSTART, 1);
  }
}

#endif // !ENABLE_NET_BLE


static void  __attribute__((constructor(132)))
nrf52_clk_init(void)
{
#ifndef ENABLE_NET_BLE
  irq_enable(0, IRQ_LEVEL_CLOCK);
  reg_wr(CLOCK_INTENSET,
         (1 << 4) | // Calibration interval Timeout
         (1 << 3) | // Calibration done
         (1 << 0)); // HF Started
#endif

  reg_wr(CLOCK_TASKS_LFCLKSTART, 1);
  while(reg_rd(CLOCK_EVENTS_LFCLKSTARTED) == 0) {}

#ifndef ENABLE_NET_BLE
  reg_wr(CLOCK_CTIV, 0x0);
  reg_wr(CLOCK_TASKS_CTSTART, 1);
  reg_wr(CLOCK_CTIV, 0x7f); // Recalibrate LFCLK every 31.75 second
#endif
}
