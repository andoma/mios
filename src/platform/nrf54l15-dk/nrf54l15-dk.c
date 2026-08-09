#include <stdio.h>
#include <unistd.h>

#include <string.h>

#include <mios/io.h>
#include <mios/mcp.h>
#include <mios/task.h>
#include <mios/cli.h>
#include <mios/error.h>
#include <mios/hostname.h>
#include <mios/type_macros.h>

#include <mios/block.h>
#include <mios/fs.h>
#include <drivers/spiflash.h>

#include "nrf54l_uart.h"
#include "nrf54l_spi.h"
#include "nrf54l_reg.h"
#include "nrf_sdc.h"

// REGULATORS (secure) VREGMAIN.DCDCEN. The DK is fitted with the VREGMAIN
// inductor, so run the main supply from the DC/DC buck: better efficiency and,
// more importantly, better load-transient response for the radio's rapid
// current bursts (e.g. Channel Sounding tone exchanges).
#define REG_VREGMAIN_DCDCEN 0x50120600

static void __attribute__((constructor(105)))
board_init_dcdc(void)
{
  reg_wr(REG_VREGMAIN_DCDCEN, 1);
}

// nRF54L15 DK onboard debugger virtual serial ports (HW User Guide v1.0),
// both exposed as host ACM ports:
//   VCOM "UART_1": TXD P1.04, RXD P1.05 -> UARTE20 (PERI domain, SERIAL20=198)
//   VCOM "UART_0": TXD P0.00, RXD P0.01 -> UARTE30 (LP domain,   SERIAL30=260)
#define UARTE20_BASE 0x500c6000
#define SERIAL20_IRQ 198
#define UARTE30_BASE 0x50104000
#define SERIAL30_IRQ 260

// Board LEDs (active high). HW User Guide Table 5.
static const gpio_t leds[] = {
  GPIO_P2(9),   // LED0
  GPIO_P1(10),  // LED1
  GPIO_P2(7),   // LED2
  GPIO_P1(14),  // LED3
};


static void __attribute__((constructor(110)))
board_init_console(void)
{
  // Primary console (default main() runs the CLI on stdio). Ctrl-D panics.
  stdio = nrf54l_uart_init(UARTE20_BASE, SERIAL20_IRQ, 115200,
                           GPIO_P1(4), GPIO_P1(5), UART_CTRLD_IS_PANIC);
}


// LED0 heartbeat = "alive" (blinks whether or not CS is running), so a
// battery-powered board is distinguishable from a dead one at a glance.
static void *
heartbeat_thread(void *arg)
{
  int on = 0;
  while(1) {
    usleep(500000);
    on = !on;
    gpio_set_output(leds[0], on);
  }
  return NULL;
}

static void __attribute__((constructor(5200)))
board_init_heartbeat(void)
{
  thread_create(heartbeat_thread, NULL, 1024, "hb", TASK_DETACHED, 2);
}

// LED1 = "ranging": toggled on each Channel Sounding update (weak no-op default
// in nrf_sdc_cs.c). On the battery reflector this blinks while it is actively
// ranging, and goes quiet (LED1 stops) when the CS link drops. Distinct from
// LED0's alive heartbeat.
void
board_cs_activity(void)
{
  static uint8_t on;
  on = !on;
  gpio_set_output(leds[1], on);
}


static void __attribute__((constructor(800)))
board_init_late(void)
{
  // Drive the LEDs as outputs (off); no animation.
  for(size_t i = 0; i < ARRAYSIZE(leds); i++)
    gpio_conf_output(leds[i], GPIO_PUSH_PULL, GPIO_SPEED_LOW, GPIO_PULL_NONE);

  // MCP (structured, HDLC-framed) on the other VCOM (UARTE30, P0.00/P0.01).
  stream_t *mcp = nrf54l_uart_init(UARTE30_BASE, SERIAL30_IRQ, 115200,
                                   GPIO_P0(0), GPIO_P0(1), 0);
  mcp_uart_create(mcp);

  // Advertise under the (persisted) hostname. nrf_ble_init only stores the
  // pointer; the controller reads it when it builds the adv data on the main
  // thread (constructor 5200), after board_init_hostname (5150) has loaded it.
  nrf_ble_init(hostname);
}


// --- External QSPI flash ---------------------------------------------------
// The DK's 64 Mbit Macronix MX25R6435F is wired to P2 and connected through
// the board controller's mux (board-config pin 47 = on). Run it as a plain
// single-bit SPI device on the HS-SPI instance SPIM00 (P2 is the MCU
// high-speed pad port). IO2(/WP)=P2.03 and IO3(/HOLD)=P2.00 are held high so
// the flash stays in 1-bit mode.
//   SCK=P2.01  MOSI(IO0)=P2.02  MISO(IO1)=P2.04  CS=P2.05
#define SPIM00_BASE  0x5004a000
#define SPIM00_IRQ   74          // SERIAL00
#define SPIM00_CLOCK 128000000   // SPIM00 source clock (128 MHz)

// Persisted hostname (littlefs). Distinguishes the two identical DKs on the
// console (and as the BLE advertised name). Stored as the bare name string.
#define HOSTNAME_PATH "/hostname"

static void
board_load_hostname(void)
{
  fs_file_t *f;
  if(!fs_open(HOSTNAME_PATH, FS_RDONLY, &f)) {
    char buf[HOSTNAME_BUFFER_SIZE];
    ssize_t n = fs_read(f, buf, sizeof(buf) - 1);
    fs_close(f);
    if(n > 0) {
      buf[n] = '\0';
      hostname_set(buf);
    }
  }
  if(hostname[0] == '\0')
    hostname_set("mios-nrf54l"); // default when nothing is stored
  printf("hostname: %s\n", hostname);
}

static error_t
cmd_hostname(cli_t *cli, int argc, char **argv)
{
  if(argc > 1) {
    hostname_set(argv[1]);
    fs_file_t *f;
    if(!fs_open(HOSTNAME_PATH, FS_WRONLY | FS_CREAT | FS_TRUNC, &f)) {
      fs_write(f, argv[1], strlen(argv[1]));
      fs_close(f);
    }
    cli_printf(cli, "hostname set (reboot to apply to the BLE adv name)\n");
  }
  cli_printf(cli, "%s\n", hostname);
  return 0;
}

CLI_CMD_DEF_EXT("hostname", cmd_hostname, NULL, "Get/set persisted hostname");

// Constructor priority > 4999 runs on the main thread after multitasking has
// started (see multitasking_mark in kernel/mios.c), so interrupts are enabled
// and the interrupt-driven, blocking spiflash_create() works. Constructors
// <= 4999 run with interrupts masked and would hang here.
static void __attribute__((constructor(5100)))
board_init_flash(void)
{
  // /WP (P2.03) and /HOLD (P2.00) high -> single-line mode.
  gpio_conf_output(GPIO_P2(3), GPIO_PUSH_PULL, GPIO_SPEED_HIGH, GPIO_PULL_NONE);
  gpio_set_output(GPIO_P2(3), 1);
  gpio_conf_output(GPIO_P2(0), GPIO_PUSH_PULL, GPIO_SPEED_HIGH, GPIO_PULL_NONE);
  gpio_set_output(GPIO_P2(0), 1);

  spi_t *spi = nrf54l_spi_create(SPIM00_BASE, SPIM00_IRQ, SPIM00_CLOCK,
                                 GPIO_P2(1),   // SCK
                                 GPIO_P2(4),   // MISO (IO1)
                                 GPIO_P2(2));  // MOSI (IO0)

  block_iface_t *blk = spiflash_create(spi, GPIO_P2(5)); // CS
  if(blk != NULL) {
    fs_init(blk); // mount littlefs (formats on first boot)
    board_load_hostname();
  }
}
