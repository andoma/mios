#include "irq.h"
#include "nrf54l_reg.h"
#include "nrf54l_radio_core.h"
#include "nrf54l_radio_arb.h"

enum {
  OWNER_NONE,
  OWNER_BLE,
  OWNER_BG,
};

static uint8_t owner;
static uint8_t have_ble;
static uint8_t have_bg;
static radio_client_t ble;
static radio_client_t bg;


void
nrf54l_radio_arb_init(void)
{
  irq_enable(RADIO_IRQ, IRQ_LEVEL_NET);
}


void
nrf54l_radio_arb_set_ble(const radio_client_t *c)
{
  ble = *c;
  have_ble = 1;
}


void
nrf54l_radio_arb_set_background(const radio_client_t *c)
{
  int q = irq_forbid(IRQ_LEVEL_NET);
  bg = *c;
  have_bg = 1;
  if(owner == OWNER_NONE) {
    nrf54l_radio_use_154();
    reg_wr(RADIO_EVENTS_END, 0);
    owner = OWNER_BG;
    if(bg.resume)
      bg.resume();
  }
  irq_permit(q);
}


void
nrf54l_radio_arb_acquire(void)
{
  int q = irq_forbid(IRQ_LEVEL_NET);
  if(owner != OWNER_BLE) {
    if(owner == OWNER_BG && bg.suspend)
      bg.suspend();
    reg_wr(RADIO_EVENTS_END, 0);
    reg_wr(RADIO_EVENTS_DISABLED, 0);
    nrf54l_radio_use_ble();
    owner = OWNER_BLE;
  }
  irq_permit(q);
}


void
nrf54l_radio_arb_release(void)
{
  int q = irq_forbid(IRQ_LEVEL_NET);
  if(owner == OWNER_BLE) {
    owner = OWNER_NONE;
    if(have_bg) {
      nrf54l_radio_use_154();
      reg_wr(RADIO_EVENTS_END, 0);
      owner = OWNER_BG;
      if(bg.resume)
        bg.resume();
    }
  }
  irq_permit(q);
}


// RADIO_0 interrupt: dispatch to whichever client currently owns the radio.
void
irq_138(void)
{
  if(owner == OWNER_BLE) {
    if(have_ble && ble.irq)
      ble.irq();
  } else if(owner == OWNER_BG) {
    if(have_bg && bg.irq)
      bg.irq();
  } else {
    reg_wr(RADIO_EVENTS_END, 0);
  }
}
