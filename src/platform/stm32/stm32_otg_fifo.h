#pragma once

// Included after the platform register accessors and OTG_GRSTCTL definition.
// Same AHB-idle -> TXFFLSH -> completion sequence as ST's USB_FlushTxFifo.
// Bounded polls: reset runs in interrupt context, never wait indefinitely.
#define OTG_FIFO_POLL_LIMIT 1024

static int
otg_flush_all_tx_fifos(void)
{
  unsigned n;
  for(n = 0; n < OTG_FIFO_POLL_LIMIT; n++) {
    if(reg_rd(OTG_GRSTCTL) & (1u << 31))
      break;
  }
  if(n == OTG_FIFO_POLL_LIMIT)
    return -1;

  reg_wr(OTG_GRSTCTL, (0x10u << 6) | (1u << 5));
  for(n = 0; n < OTG_FIFO_POLL_LIMIT; n++) {
    if(!(reg_rd(OTG_GRSTCTL) & (1u << 5)))
      return 0;
  }
  return -1;
}
