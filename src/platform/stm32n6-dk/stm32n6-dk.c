#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <mios/io.h>
#include <mios/task.h>
#include <mios/block.h>
#include <mios/cli.h>
#include "stm32n6_flash.h"

#include "stm32n6_clk.h"
#include "stm32n6_pwr.h"
#include "stm32n6_usb.h"
#include "stm32n6_eth.h"
#include "stm32n6_uart.h"
#include "stm32n6_ucpd.h"
#include "stm32n6_i2c.h"
#include "stm32n6_adc.h"

#include "camera.h"

#include <usb/usb.h>
#include <usb/usb_host.h>
#include <usb/typec.h>
#include <usb/usb_pd.h>

#include "drivers/tcpp03.h"

#define BLINK_GPIO GPIO_PG(10) // Red LED (LD2)

static void __attribute__((constructor(101)))
board_init_clk(void)
{
  stm32n6_init_pll(48000000);

  stm32n6_pwr_vddio3_enable(STM32N6_VDDIO_1V8);
  // PC1, PC[12:6], PH9 (camera I2C and reset) are supplied from VDDIO4,
  // which is tied to the 3.3V VDDIO rail on this board
  stm32n6_pwr_vddio4_enable(STM32N6_VDDIO_3V3);
  stm32n6_pwr_usb33_enable();
}

static void __attribute__((constructor(102)))
board_init_console(void)
{
  static stm32_uart_stream_t console;
  stdio = stm32n6_uart_init(&console, 1, 115200, GPIO_PE(5), GPIO_PE(6),
                            UART_CTRLD_IS_PANIC, "console");
}

__attribute__((noreturn))
static void *
blinker(void *arg)
{
  gpio_conf_output(BLINK_GPIO, GPIO_PUSH_PULL, GPIO_SPEED_LOW,
                   GPIO_PULL_NONE);
  while(1) {
    gpio_set_output(BLINK_GPIO, 1);
    usleep(500000);
    gpio_set_output(BLINK_GPIO, 0);
    usleep(500000);
  }
}

// USB-C port (CN18): dual role. OTG1 is a device when attached as sink and
// a host when attached as source. The TCPP03 on I2C2 switches VBUS
static tcpp03_t *usbc_tcpp;
static usb_pd_t *usbc_pd;

static void
usbc_role_changed(void *opaque, typec_role_t role)
{
  switch(role) {
  case TYPEC_ROLE_SOURCE:
    tcpp03_source_on(usbc_tcpp);
    stm32n6_otghs1_host_enable(1);
    break;
  case TYPEC_ROLE_SINK:
    tcpp03_sink_on(usbc_tcpp);
    stm32n6_otghs_device_enable(1);
    break;
  case TYPEC_ROLE_NONE:
    // VBUS off first, then release the core
    tcpp03_detach(usbc_tcpp);
    stm32n6_otghs1_host_enable(0);
    stm32n6_otghs_device_enable(0);
    break;
  }
}


// VBUS on PA11 (ADC1 channel 12) through a 140k / 10k divider
static int
usbc_vbus_mv(void *opaque)
{
  const int raw = stm32n6_adc_read_channel(ADC1_BASE, 12);
  return raw * ADC_VREF_MV * 15 / 4095;
}


// The TCPP03 needs I2C and delays, which can't be done from constructors
static void *
usbc_init_thread(void *arg)
{
  i2c_t *i2c = arg;
  usbc_tcpp = tcpp03_create(i2c, 0x34, GPIO_PA(4), "tcpp03");
  tcpp03_set_vbus_monitor(usbc_tcpp, usbc_vbus_mv, NULL);
  tcpp03_set_flag_gpio(usbc_tcpp, GPIO_PD(10));

  typec_phy_t *phy = stm32n6_ucpd_create();

  // The TCPP03 overvoltage protection trips at about 6V (R193 / R192).
  // It then also disconnects CC, so a higher voltage contract never
  // completes: VBUS gets there, but the source's PS_RDY can't get
  // through and the source resets the port. Stay at 5V
  usbc_pd = usb_pd_create(phy, "usb-pd");
  usb_pd_set_sink_policy(usbc_pd, 5000, 1500);

  const typec_port_config_t cfg = {
    .type = TYPEC_PORT_DRP,
    .prefer_sink = 1,
    .rp = TYPEC_PULL_RP_DEF,
    .role_changed = usbc_role_changed,
    .vbus_mv = usbc_vbus_mv,
    .pd = usbc_pd,
  };
  typec_port_create(phy, &cfg, "usb-c");
  return NULL;
}


static void
usbc_init(void)
{
  // Device mode waits for Type-C to attach us as a sink
  stm32n6_otghs_device_enable(0);
  stm32n6_otghs1_host_create();


  // The channel preselect must be set before the ADC is enabled. The pin
  // must be non-secure, or the GPIO keeps the (non-secure) ADC's analog
  // switch open
  gpio_conf_analog(GPIO_PA(11), GPIO_PULL_NONE);
  stm32n6_gpio_set_secure(GPIO_PA(11), 0);
  stm32n6_adc_init(ADC1_BASE, 1 << 12, 0);
  stm32n6_adc_config_input(ADC1_BASE, 12);
  i2c_t *i2c = stm32n6_i2c_create(2, GPIO_PD(14), GPIO_PD(4),
                                  GPIO_PULL_UP, 100000);
  thread_create(usbc_init_thread, i2c, 1024, "usbcinit", TASK_DETACHED, 4);
}


static void __attribute__((constructor(800)))
board_init_late(void)
{
  camera_init();

  thread_create(blinker, NULL, 512, "blinker", 0, 0);

  struct usb_interface_queue q;
  STAILQ_INIT(&q);

  usb_cdc_create_shell(&q);

  stm32n6_otghs_create(0x6666, 0x0500, "Lonelycoder", "stm32n6-dk", &q);
  usbc_init();

  // USB-A host port (CN17), VBUS switch enabled by PB9
  usb_host_acm_register();
  stm32n6_otghs2_host_create(GPIO_PB(9));

  stm32n6_eth_init(GPIO_UNUSED, GPIO_PD(12), GPIO_PD(1),
                   1, ETHPHY_MODE_RGMII);
}

// Set the USB PD sink policy: highest fixed voltage and current to request
static error_t
cmd_pd(cli_t *cli, int argc, char **argv)
{
  if(argc < 2 || usbc_pd == NULL)
    return ERR_INVALID_ARGS;
  const int mv = atoi(argv[1]);
  const int ma = argc > 2 ? atoi(argv[2]) : 1500;
  usb_pd_set_sink_policy(usbc_pd, mv, ma);
  return 0;
}

CLI_CMD_DEF("pd", cmd_pd);


// Run a command line on the shell of an attached CDC-ACM device and print
// what comes back. With CN17 cabled to CN18 that is this board's own USB
// shell
static error_t
cmd_acm(cli_t *cli, int argc, char **argv)
{
  if(argc < 2)
    return ERR_INVALID_ARGS;

  stream_t *s = usb_host_acm_open(0);
  if(s == NULL) {
    cli_printf(cli, "No CDC-ACM device\n");
    return ERR_NO_DEVICE;
  }

  for(int i = 1; i < argc; i++) {
    stream_write(s, argv[i], strlen(argv[i]), 0);
    stream_write(s, i == argc - 1 ? "\r" : " ", 1, 0);
  }

  // Print the output until it has been quiet for 200ms
  char buf[64];
  error_t err = 0;
  int64_t deadline = clock_get() + 200000;
  while(clock_get() < deadline) {
    const ssize_t r = stream_read(s, buf, sizeof(buf), 0);
    if(r < 0) {
      err = r;
      break;
    }
    if(r == 0) {
      usleep(10000);
      continue;
    }
    stream_write(cli->cl_stream, buf, r, 0);
    deadline = clock_get() + 200000;
  }
  stream_close(s);
  cli_printf(cli, "\n");
  return err;
}

CLI_CMD_DEF("acm", cmd_acm);


// Embedded FSBL image generated by STM32_SigningTool_CLI
#include "fsbl_image.h"

static error_t
write_fsbl_to_partition(cli_t *cli, int partition, const char *name)
{
  block_iface_t *bi = stm32n6_flash_get_partition(partition);
  if(bi == NULL) {
    cli_printf(cli, "%s: partition not available\n", name);
    return ERR_OPERATION_FAILED;
  }

  const size_t len = build_stm32n6_dk_fsbl_bin_len;
  size_t blocks_needed = (len + bi->block_size - 1) / bi->block_size;

  cli_printf(cli, "%s: %d bytes\n", name, (int)len);

  error_t err = block_erase(bi, 0, blocks_needed);
  if(err) { cli_printf(cli, "  Erase failed: %d\n", err); return err; }

  err = block_write(bi, 0, 0, build_stm32n6_dk_fsbl_bin, len);
  if(err) { cli_printf(cli, "  Write failed: %d\n", err); return err; }

  cli_printf(cli, "  OK\n");
  return 0;
}

static error_t
cmd_install_bootloader(cli_t *cli, int argc, char **argv)
{
  error_t err;

  err = write_fsbl_to_partition(cli, FLASH_PARTITION_FSBL1, "FSBL1");
  if(err) return err;

  err = write_fsbl_to_partition(cli, FLASH_PARTITION_FSBL2, "FSBL2");
  if(err) return err;

  cli_printf(cli, "Done. Power cycle to boot from flash.\n");
  return 0;
}

CLI_CMD_DEF("install-bootloader", cmd_install_bootloader);
