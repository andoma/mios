#include "irq.h"
#include "nrf54l_reg.h"
#include "nrf54l_radio_core.h"
#include "nrf54l_radio_arb.h"
#include "nrf54l_radio.h" // nrf54l_ble_radio_irq
#include "nrf54l_154.h"   // nrf54l_154_irq / _suspend / _resume

enum {
  OWNER_NONE,
  OWNER_BLE,
  OWNER_BG, // 802.15.4 background RX
};

static uint8_t owner;


void
nrf54l_radio_arb_init(void)
{
  irq_enable(RADIO_IRQ, IRQ_LEVEL_NET);
}


void
nrf54l_radio_arb_acquire(void)
{
  int q = irq_forbid(IRQ_LEVEL_NET);
  if(owner != OWNER_BLE) {
    if(owner == OWNER_BG)
      nrf54l_154_suspend();
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
    nrf54l_radio_use_154();
    reg_wr(RADIO_EVENTS_END, 0);
    owner = OWNER_BG;
    nrf54l_154_resume();
  }
  irq_permit(q);
}


void
nrf54l_radio_arb_background_start(void)
{
  int q = irq_forbid(IRQ_LEVEL_NET);
  if(owner == OWNER_NONE) {
    nrf54l_radio_use_154();
    reg_wr(RADIO_EVENTS_END, 0);
    owner = OWNER_BG;
    nrf54l_154_resume();
  }
  irq_permit(q);
}


// RADIO_0 interrupt: dispatch to whichever client currently owns the radio.
void
irq_138(void)
{
  if(owner == OWNER_BLE)
    nrf54l_ble_radio_irq();
  else if(owner == OWNER_BG)
    nrf54l_154_irq();
  else
    reg_wr(RADIO_EVENTS_END, 0);
}
