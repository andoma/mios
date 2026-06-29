// 802.15.4 background-RX client. Registers with the radio arbiter and receives
// whenever BLE is not using the radio. It does NOT log every frame (a busy
// Thread mesh would flood the console and exhaust the pbuf pool); instead it
// counts frames and keeps the most recent one, dumped on demand via the '154'
// CLI command. This is the Thread-side half of the concurrent BLE + 15.4
// milestone; for now it only listens.

#include <stdio.h>
#include <string.h>

#include <sys/param.h>

#include <mios/mios.h>
#include <mios/cli.h>
#include <mios/device.h>

#include "irq.h"
#include "nrf54l_reg.h"
#include "nrf54l_radio_core.h"
#include "nrf54l_radio_arb.h"

// The local Thread network's channel (from sniffing: Apple-TV mesh on ch25).
#define CH154 25

// PHR length byte + up to 127 byte PSDU.
static uint8_t rxbuf[132] __attribute__((aligned(4)));

static volatile uint32_t rx_total;
static volatile uint32_t rx_ok;
static uint8_t last_frame[32];
static volatile uint8_t last_len;


static void
rx154_resume(void)
{
  reg_wr(RADIO_FREQUENCY, 5 + 5 * (CH154 - 11));
  reg_wr(RADIO_PACKETPTR, (intptr_t)rxbuf);
  reg_wr(RADIO_EVENTS_END, 0);
  reg_wr(RADIO_TASKS_RXEN, 1); // READY_START + END_START shorts -> continuous RX
}


static void
rx154_suspend(void)
{
  reg_wr(RADIO_EVENTS_DISABLED, 0);
  reg_wr(RADIO_TASKS_DISABLE, 1);
  while(!reg_rd(RADIO_EVENTS_DISABLED)) {}
}


static void
rx154_irq(void)
{
  if(!reg_rd(RADIO_EVENTS_END))
    return;
  reg_wr(RADIO_EVENTS_END, 0); // END_START has already restarted RX

  rx_total++;
  if(reg_rd(RADIO_CRCSTATUS)) {
    rx_ok++;
    int len = rxbuf[0];
    if(len > 1 && len <= 127) {
      // Snapshot the head of the most recent good frame for the '154' command.
      uint8_t n = MIN(len, (int)sizeof(last_frame));
      memcpy(last_frame, rxbuf + 1, n);
      last_len = n;
    }
  }
}


static const radio_client_t client = {
  .suspend = rx154_suspend,
  .resume = rx154_resume,
  .irq = rx154_irq,
};


static void
ieee154_print_info(struct device *dev, struct stream *st)
{
  int q = irq_forbid(IRQ_LEVEL_NET);
  uint32_t total = rx_total, ok = rx_ok;
  uint8_t n = last_len, buf[32];
  memcpy(buf, last_frame, n);
  irq_permit(q);

  stprintf(st, "Channel: %d  RX frames: %u  FCS-ok: %u\n", CH154,
           (unsigned)total, (unsigned)ok);
  if(n) {
    stprintf(st, "Last frame: ");
    for(int i = 0; i < n; i++)
      stprintf(st, "%02x ", buf[i]);
    stprintf(st, "\n");
  }
}


static const device_class_t ieee154_device_class = {
  .dc_class_name = "ieee802154",
  .dc_print_info = ieee154_print_info,
};

static device_t ieee154_dev;


static error_t
cmd_154(cli_t *cli, int argc, char **argv)
{
  uint8_t buf[32];
  uint8_t n;
  int q = irq_forbid(IRQ_LEVEL_NET);
  uint32_t total = rx_total, ok = rx_ok;
  n = last_len;
  memcpy(buf, last_frame, n);
  irq_permit(q);

  cli_printf(cli, "802.15.4 ch%d: %u frames, %u FCS-ok\n", CH154,
             (unsigned)total, (unsigned)ok);
  if(n) {
    cli_printf(cli, "last: ");
    for(int i = 0; i < n; i++)
      cli_printf(cli, "%02x ", buf[i]);
    cli_printf(cli, "\n");
  }
  return 0;
}

CLI_CMD_DEF("154", cmd_154);


void
nrf54l_154_init(void)
{
  nrf54l_radio_arb_set_background(&client);

  ieee154_dev.d_name = "ieee802154";
  ieee154_dev.d_class = &ieee154_device_class;
  ieee154_dev.d_parent = nrf54l_radio_parent();
  device_register(&ieee154_dev);

  printf("802.15.4 background RX on ch %d\n", CH154);
}
