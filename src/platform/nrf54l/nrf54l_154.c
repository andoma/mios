// 802.15.4 background-RX client. Driven by the radio arbiter: it receives
// whenever BLE is not using the radio. It does not log every frame (a busy
// Thread mesh would flood the console); it counts frames and keeps the most
// recent one, shown under the 'radio' device in 'dev'. RX only, for now.

#include <stdio.h>

#include <mios/mios.h>

#include "irq.h"
#include "nrf54l_reg.h"
#include "nrf54l_radio_core.h"
#include "nrf54l_radio_arb.h"
#include "nrf54l_154.h"

// The local Thread network's channel (from sniffing: Apple-TV mesh on ch25).
#define CH154 25

// PHR length byte + up to 127 byte PSDU.
static uint8_t rxbuf[132] __attribute__((aligned(4)));

static volatile uint32_t rx_total;
static volatile uint32_t rx_ok;


// Arbiter granted us the radio (already in 15.4 PHY); start continuous RX.
void
nrf54l_154_resume(void)
{
  reg_wr(RADIO_FREQUENCY, 5 + 5 * (CH154 - 11));
  reg_wr(RADIO_PACKETPTR, (intptr_t)rxbuf);
  reg_wr(RADIO_EVENTS_END, 0);
  reg_wr(RADIO_TASKS_RXEN, 1); // READY_START + END_START shorts -> continuous RX
}


// Arbiter is taking the radio back for BLE; stop receiving.
void
nrf54l_154_suspend(void)
{
  reg_wr(RADIO_EVENTS_DISABLED, 0);
  reg_wr(RADIO_TASKS_DISABLE, 1);
  while(!reg_rd(RADIO_EVENTS_DISABLED)) {}
}


// Radio interrupt while we own the radio: a frame has been received.
void
nrf54l_154_irq(void)
{
  if(!reg_rd(RADIO_EVENTS_END))
    return;
  reg_wr(RADIO_EVENTS_END, 0); // END_START has already restarted RX

  rx_total++;
  if(reg_rd(RADIO_CRCSTATUS))
    rx_ok++;
}


void
nrf54l_154_print(struct stream *st)
{
  stprintf(st, "802.15.4: channel %d  RX:%u  FCS-ok:%u\n", CH154,
           (unsigned)rx_total, (unsigned)rx_ok);
}


void
nrf54l_154_init(void)
{
  nrf54l_radio_arb_background_start();
  printf("802.15.4 background RX on ch %d\n", CH154);
}
