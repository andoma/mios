#include <mios/smux.h>
#include <mios/stream.h>
#include <mios/pipe.h>

#include <stdio.h>

#include "drivers/uart_16550.h"
#include "tegra234_hsp.h"
#include "irq.h"

#define TCU_CHANNEL_SPE    0xe0
#define TCU_CHANNEL_CCPLEX 0xe1
#define TCU_CHANNEL_BPMP   0xe2
#define TCU_CHANNEL_SCE    0xe3
#define TCU_CHANNEL_TZ     0xe4
#define TCU_CHANNEL_RCE    0xe5

// TCU init. Signal RESET + SPE output
static const uint8_t tcu_reset_seq[] = {0xff, 0xfd, 0xff, 0xe0};

stream_t *g_uartc;

static stream_t *spe_tcu_console;

static stream_t *
spe_console_stream(void)
{
  static uint8_t spe_console_can_sleep;

  if(!spe_tcu_console)
    return g_uartc;

  int s = can_sleep();
  if(s != spe_console_can_sleep) {
    spe_console_can_sleep = s;
    stream_write(g_uartc, tcu_reset_seq, sizeof(tcu_reset_seq), 0);
  }
  return s ? spe_tcu_console : g_uartc;
}

static ssize_t
spe_stdio_read(struct stream *s, void *buf, size_t size, size_t required)
{
  return stream_read(spe_console_stream(), buf, size, required);
}


static ssize_t
spe_stdio_write(struct stream *s, const void *buf, size_t size, int flags)
{
  return stream_write(spe_console_stream(), buf, size, flags);
}


static const stream_vtable_t spe_stdio_vtable = {
  .read = spe_stdio_read,
  .write = spe_stdio_write,
};

stream_t spe_stdio = {
  .vtable = &spe_stdio_vtable
};


static void  __attribute__((constructor(108)))
tcu_init_early(void)
{
  g_uartc = uart_16550_create(0x0c280000, IRQ_UART_1);
  stdio = &spe_stdio;

  stream_write(stdio, tcu_reset_seq, sizeof(tcu_reset_seq), 0);
  printf("\nTCU console initialized\n");
}


static void  __attribute__((constructor(5000)))
tcu_init_late(void)
{
  stream_t *svec[6];

  // SPE (this is us)
  pipe(&svec[0], &spe_tcu_console);

  // CCPLEX
  svec[1] = hsp_mbox_stream(NV_ADDRESS_MAP_AON_HSP_BASE, 1,
                            NV_ADDRESS_MAP_TOP0_HSP_BASE, 0);

  // BPMP
  svec[2] = hsp_mbox_stream(NV_ADDRESS_MAP_AON_HSP_BASE, 6, 0, 0);

  // DCE
  svec[3] = hsp_mbox_stream(NV_ADDRESS_MAP_AON_HSP_BASE, 3, 0, 0);

  // TZ
  svec[4] = hsp_mbox_stream(NV_ADDRESS_MAP_AON_HSP_BASE, 7, 0, 0);

  // RCE
  svec[5] = hsp_mbox_stream(NV_ADDRESS_MAP_AON_HSP_BASE, 5, 0, 0);

  static const uint8_t tcuids[6] = {
    TCU_CHANNEL_SPE,
    TCU_CHANNEL_CCPLEX,
    TCU_CHANNEL_BPMP,
    TCU_CHANNEL_SCE,
    TCU_CHANNEL_TZ,
    TCU_CHANNEL_RCE,
  };

  smux_create(g_uartc, 0xff, 0xfd, 6, tcuids, svec, 0);
}
