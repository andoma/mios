#include "nrf52_clk.h"
#include "nrf52_reg.h"
#include "nrf52_radio.h"

#include "irq.h"

#include "net/netif.h"

static uint8_t xtal_want;
static uint8_t xtal_have;

#define XTAL_CALIB 0x1
#define XTAL_RADIO 0x2


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
  if(got & XTAL_RADIO)
    nrf52_radio_got_xtal();
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


static void  __attribute__((constructor(132)))
nrf52_clk_init(void)
{
  irq_enable(0, IRQ_LEVEL_CLOCK);
  reg_wr(CLOCK_INTENSET,
         (1 << 4) | // Calibration interval Timeout
         (1 << 3) | // Calibration done
         (1 << 0)); // HF Started

  reg_wr(CLOCK_TASKS_LFCLKSTART, 1);
  while(reg_rd(CLOCK_EVENTS_LFCLKSTARTED) == 0) {}

  reg_wr(CLOCK_CTIV, 0x0);
  reg_wr(CLOCK_TASKS_CTSTART, 1);
  reg_wr(CLOCK_CTIV, 0x7f); // Recalibrate LFCLK every 31.75 second
}



void
nrf52_clk_radio_xtal(int on)
{
  if(on)
    xtal_set(XTAL_RADIO);
  else
    xtal_clear(XTAL_RADIO);
}
