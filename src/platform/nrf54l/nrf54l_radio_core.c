// Radio core: HFXO control and the BLE PHY preset for the native link layer.

#include "nrf54l_reg.h"
#include "nrf54l_radio_core.h"

#define CLOCK_BASE             0x5010e000
#define CLOCK_TASKS_XOSTART    (CLOCK_BASE + 0x000)
#define CLOCK_EVENTS_XOSTARTED (CLOCK_BASE + 0x100)

static uint8_t hfxo_running;


void
nrf54l_hfxo_start(void)
{
  if(hfxo_running)
    return;
  reg_wr(CLOCK_EVENTS_XOSTARTED, 0);
  reg_wr(CLOCK_TASKS_XOSTART, 1);
  while(!reg_rd(CLOCK_EVENTS_XOSTARTED)) {}
  hfxo_running = 1;
}


void
nrf54l_radio_use_ble(void)
{
  reg_wr(RADIO_MODE, 3); // 1 Mbit/s BLE
  reg_wr(RADIO_CRCPOLY, 0x65b);
  reg_wr(RADIO_CRCCNF,
         (1 << 8) | // skip the access address in the CRC
         (3 << 0)); // 3-byte CRC

  reg_wr(RADIO_RXADDRESSES, 0x1); // logical address 0
  reg_wr(RADIO_TXADDRESS, 0);

  reg_wr(RADIO_PCNF0,
         (8 << 0)  | // LFLEN: 8-bit length field
         (1 << 8)  | // S0LEN: 1 byte (the PDU header)
         (0 << 16)); // S1LEN: none

  // MAXLEN must not exceed the RX pbuf payload (PBUF_DATA_SIZE), or a received
  // packet's EasyDMA write overruns the pbuf into the heap. LLMTU is exactly
  // that bound (PBUF_DATA_SIZE - 2); the BLE link-layer never sends/expects
  // more in a single PDU.
  reg_wr(RADIO_PCNF1,
         ((PBUF_DATA_SIZE - 2) << 0) | // MAXLEN = LLMTU
         (0 << 8)   | // STATLEN
         (3 << 16)  | // BALEN = 4 (3 + 1)
         (1 << 25)); // enable whitening

  reg_wr(RADIO_INTENSET00, RADIO_INT_END);
  reg_wr(RADIO_SHORTS, RADIO_SHORT_READY_START | RADIO_SHORT_PHYEND_DISABLE);
  reg_wr(RADIO_TXPOWER, 0x18); // 0 dBm (higher settings may need extra PA/regulator config)
}

