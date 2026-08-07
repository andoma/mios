// Semtech SX1280 2.4GHz transceiver
//
// This file implements the transport layer (SPI command interface,
// BUSY handshake, reset and DIO1 interrupt plumbing) and a CLI
// command for bring-up. Modem/MAC layers build on top of this.

#include "sx1280.h"
#include "sx1280_i.h"
#include "sx1280_ble.h"
#include "sx1280_sched.h"

#include <mios/cli.h>
#include <mios/eventlog.h>
#include <mios/task.h>

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "irq.h"

// BUSY is normally released within µs, but first wakeup from cold
// reset (crystal + calibration) is spec'd in ms
#define SX1280_BUSY_TIMEOUT   100000 // µs
#define SX1280_RESET_TIMEOUT  200000 // µs

// Single instance for CLI access
static sx1280_t *sx1280_cli_instance;

static void
sx1280_busy_irq(void *arg)
{
  sx1280_t *s = arg;
  task_wakeup(&s->busy_waitq, 1);
}

static void
sx1280_dio1_irq(void *arg)
{
  sx1280_t *s = arg;
  s->irq_pending = 1;
  task_wakeup(&s->irq_waitq, 1);
}

// Command processing takes µs, so spin briefly first. For the long
// waits (reset, calibration) sleep with the BUSY IRQ masked across
// the level-check-and-sleep: the falling edge cannot fire between the
// check and the waitqueue enrollment, so no wakeup is ever lost. The
// mask is per-thread state; it does not stay raised while sleeping.
static error_t
sx1280_wait_ready(sx1280_t *s, int timeout)
{
  const uint64_t spin_until = clock_get() + 30;
  while(gpio_get_input(s->busy)) {
    if(clock_get() < spin_until)
      continue;

    const int64_t deadline = clock_get() + timeout;
    error_t err = ERR_OK;
    int q = irq_forbid(IRQ_LEVEL_IO);
    while(gpio_get_input(s->busy)) {
      if(task_sleep_deadline(&s->busy_waitq, deadline)) {
        err = ERR_TIMEOUT;
        break;
      }
    }
    irq_permit(q);
    return err;
  }
  return ERR_OK;
}

error_t
sx1280_cmd(sx1280_t *s, const uint8_t *tx, uint8_t *rx, size_t len)
{
  error_t err = sx1280_wait_ready(s, SX1280_BUSY_TIMEOUT);
  if(err)
    return err;
  return s->bus->rw(s->bus, tx, rx, len, s->nss, s->spicfg);
}

static int
sx1280_irq_ack(sx1280_t *s)
{
  uint8_t st[4] = {SX1280_GET_IRQSTATUS};
  error_t err = sx1280_cmd(s, st, st, sizeof(st));
  if(err)
    return err;
  const uint16_t irq = st[2] << 8 | st[3];

  const uint8_t clr[3] = {SX1280_CLR_IRQSTATUS, irq >> 8, irq};
  err = sx1280_cmd(s, clr, NULL, sizeof(clr));
  if(err)
    return err;
  return irq;
}

int
sx1280_wait_irq(sx1280_t *s, int timeout)
{
  const int64_t deadline = clock_get() + timeout;
  int q = irq_forbid(IRQ_LEVEL_IO);
  // Also check the DIO1 level: the edge flag misses interrupts that
  // were left partially unacked (DIO1 never fell, so no new edge)
  while(!s->irq_pending && !gpio_get_input(s->dio1)) {
    if(task_sleep_deadline(&s->irq_waitq, deadline)) {
      irq_permit(q);
      return 0;
    }
  }
  s->irq_pending = 0;
  irq_permit(q);
  return sx1280_irq_ack(s);
}

// Spin-polling variant for T_IFS-critical windows where even the
// wakeup latency of the sleeping wait is unaffordable. DIO1 is
// level-high until the IRQ is cleared, so polling is race-free.
int
sx1280_wait_irq_poll(sx1280_t *s, int timeout)
{
  const uint64_t deadline = clock_get() + timeout;
  while(!gpio_get_input(s->dio1)) {
    if(clock_get() > deadline)
      return 0;
  }
  s->irq_pending = 0;
  return sx1280_irq_ack(s);
}

// Sleep until DIO1 asserts, without touching the chip (no IRQ status
// read or clear: the caller may have an AutoTx counting down, which a
// ClrIrqStatus would cancel). Returns 1 on assertion, 0 on deadline.
int
sx1280_wait_dio1(sx1280_t *s, int64_t deadline)
{
  int q = irq_forbid(IRQ_LEVEL_IO);
  while(!s->irq_pending && !gpio_get_input(s->dio1)) {
    if(task_sleep_deadline(&s->irq_waitq, deadline)) {
      irq_permit(q);
      return 0;
    }
  }
  s->irq_pending = 0;
  irq_permit(q);
  return 1;
}

error_t
sx1280_read_reg(sx1280_t *s, uint16_t addr, void *ptr, size_t len)
{
  uint8_t buf[len + 4];
  buf[0] = SX1280_READ_REGISTER;
  buf[1] = addr >> 8;
  buf[2] = addr;
  buf[3] = 0;
  error_t err = sx1280_cmd(s, buf, buf, len + 4);
  if(!err)
    memcpy(ptr, buf + 4, len);
  return err;
}

error_t
sx1280_write_reg(sx1280_t *s, uint16_t addr, const void *ptr, size_t len)
{
  uint8_t buf[len + 3];
  buf[0] = SX1280_WRITE_REGISTER;
  buf[1] = addr >> 8;
  buf[2] = addr;
  memcpy(buf + 3, ptr, len);
  return sx1280_cmd(s, buf, NULL, len + 3);
}

error_t
sx1280_reset(sx1280_t *s)
{
  gpio_set_output(s->nreset, 0);
  usleep(1000);
  gpio_set_output(s->nreset, 1);
  usleep(1000);
  return sx1280_wait_ready(s, SX1280_RESET_TIMEOUT);
}

static int
sx1280_get_status(sx1280_t *s)
{
  uint8_t buf[1] = {SX1280_GET_STATUS};
  error_t err = sx1280_cmd(s, buf, buf, 1);
  return err ?: buf[0];
}

sx1280_t *
sx1280_create(spi_t *bus, gpio_t nss, gpio_t nreset,
              gpio_t busy, gpio_t dio1, const char *name)
{
  sx1280_t *s = calloc(1, sizeof(sx1280_t));
  s->bus = bus;
  s->nss = nss;
  s->nreset = nreset;
  s->busy = busy;
  s->dio1 = dio1;
  s->name = name;

  mutex_init(&s->mutex, name);
  task_waitable_init(&s->busy_waitq, name);
  task_waitable_init(&s->irq_waitq, name);

  // Conservative clock for dupont wires; the chip does 18MHz
  s->spicfg = bus->get_config(bus, 0, 8000000);

  gpio_set_output(nss, 1);
  gpio_conf_output(nss, GPIO_PUSH_PULL, GPIO_SPEED_HIGH, GPIO_PULL_NONE);

  gpio_set_output(nreset, 1);
  gpio_conf_output(nreset, GPIO_PUSH_PULL, GPIO_SPEED_LOW, GPIO_PULL_NONE);

  gpio_conf_irq(busy, GPIO_PULL_NONE, sx1280_busy_irq, s,
                GPIO_FALLING_EDGE, IRQ_LEVEL_IO);

  gpio_conf_irq(dio1, GPIO_PULL_DOWN, sx1280_dio1_irq, s,
                GPIO_RISING_EDGE, IRQ_LEVEL_IO);

  sx1280_sched_init(s);

  sx1280_cli_instance = s;
  return s;
}


static const char *sx1280_modestr[8] = {
  "?0", "?1", "STDBY_RC", "STDBY_XOSC", "FS", "RX", "TX", "?7"
};

static const char *sx1280_cmdstatusstr[8] = {
  "?0", "ok", "data-avail", "timeout", "processing-error", "exec-fail",
  "tx-done", "?7"
};

static error_t
cmd_sx1280_test(cli_t *cli)
{
  sx1280_t *s = sx1280_cli_instance;
  error_t err;

  if((err = sx1280_reset(s)) != 0) {
    cli_printf(cli, "reset: BUSY never released: %s\n", error_to_string(err));
    return err;
  }
  cli_printf(cli, "reset: ok\n");

  const uint8_t standby[2] = {SX1280_SET_STANDBY, SX1280_STDBY_RC};
  if((err = sx1280_cmd(s, standby, NULL, sizeof(standby))) != 0)
    return err;

  int status = sx1280_get_status(s);
  if(status < 0)
    return status;

  cli_printf(cli, "status: 0x%02x mode:%s cmd:%s\n", status,
             sx1280_modestr[(status >> 5) & 7],
             sx1280_cmdstatusstr[(status >> 2) & 7]);

  // Register write/read round-trip proves full-duplex SPI + register
  // access. FLRC syncword1 is a plain 4-byte r/w register.
  const uint8_t wr[4] = {0xca, 0xfe, 0xf0, 0x0d};
  uint8_t rd[4] = {0};

  if((err = sx1280_write_reg(s, SX1280_REG_FLRC_SYNCWORD1, wr, 4)) != 0)
    return err;
  if((err = sx1280_read_reg(s, SX1280_REG_FLRC_SYNCWORD1, rd, 4)) != 0)
    return err;

  if(memcmp(wr, rd, 4)) {
    cli_printf(cli, "register round-trip FAILED: "
               "wrote %02x%02x%02x%02x read %02x%02x%02x%02x\n",
               wr[0], wr[1], wr[2], wr[3], rd[0], rd[1], rd[2], rd[3]);
    return ERR_INVALID_ID;
  }
  cli_printf(cli, "register round-trip: ok\n");
  return 0;
}


static error_t
cmd_sx1280_cw(cli_t *cli, uint32_t freq_hz)
{
  sx1280_t *s = sx1280_cli_instance;
  error_t err;

  if((err = sx1280_reset(s)) != 0)
    return err;

  const uint8_t standby[2] = {SX1280_SET_STANDBY, SX1280_STDBY_RC};
  if((err = sx1280_cmd(s, standby, NULL, sizeof(standby))) != 0)
    return err;

  const uint32_t f = SX1280_HZ_TO_FREQ(freq_hz);
  const uint8_t freq[4] = {SX1280_SET_RFFREQUENCY, f >> 16, f >> 8, f};
  if((err = sx1280_cmd(s, freq, NULL, sizeof(freq))) != 0)
    return err;

  // 0dBm (18 = -18dBm + 18), 20µs ramp
  const uint8_t txparams[3] = {SX1280_SET_TXPARAMS, 18, 0xe0};
  if((err = sx1280_cmd(s, txparams, NULL, sizeof(txparams))) != 0)
    return err;

  const uint8_t cw[1] = {SX1280_SET_TXCONTINUOUSWAVE};
  if((err = sx1280_cmd(s, cw, NULL, sizeof(cw))) != 0)
    return err;

  cli_printf(cli, "Transmitting carrier at %d Hz, 0dBm. "
             "'sx1280 test' to stop\n", (int)freq_hz);
  return 0;
}


static const char *
pin_probe(gpio_t pin)
{
  gpio_conf_input(pin, GPIO_PULL_UP);
  usleep(1000);
  int up = gpio_get_input(pin);
  gpio_conf_input(pin, GPIO_PULL_DOWN);
  usleep(1000);
  int down = gpio_get_input(pin);
  gpio_conf_input(pin, GPIO_PULL_NONE);

  if(up && !down)
    return "floating";
  return up ? "high" : "low";
}

static error_t
cmd_sx1280_pins(cli_t *cli)
{
  sx1280_t *s = sx1280_cli_instance;

  cli_printf(cli, "nreset=1: BUSY:%s DIO1:%s\n",
             pin_probe(s->busy), pin_probe(s->dio1));

  gpio_set_output(s->nreset, 0);
  usleep(2000);
  cli_printf(cli, "nreset=0: BUSY:%s DIO1:%s\n",
             pin_probe(s->busy), pin_probe(s->dio1));

  gpio_set_output(s->nreset, 1);
  usleep(20000);
  cli_printf(cli, "nreset=1: BUSY:%s DIO1:%s (20ms after release)\n",
             pin_probe(s->busy), pin_probe(s->dio1));

  // The EXTI registration from sx1280_create() survives the mode
  // changes done by pin_probe(), only the pulls need restoring
  gpio_conf_input(s->busy, GPIO_PULL_NONE);
  gpio_conf_input(s->dio1, GPIO_PULL_DOWN);
  return 0;
}

// Raw SPI transactions bypassing the BUSY handshake, for bring-up
// diagnostics when BUSY looks stuck
static error_t
cmd_sx1280_status(cli_t *cli)
{
  sx1280_t *s = sx1280_cli_instance;

  uint8_t st[2] = {SX1280_GET_STATUS, 0};
  error_t err = s->bus->rw(s->bus, st, st, sizeof(st), s->nss, s->spicfg);
  if(err)
    return err;
  cli_printf(cli, "GetStatus (no busy wait): %02x %02x  BUSY:%d\n",
             st[0], st[1], gpio_get_input(s->busy));

  uint8_t rd[8] = {SX1280_READ_REGISTER, 0x08, 0x91, 0}; // LNA regime
  err = s->bus->rw(s->bus, rd, rd, sizeof(rd), s->nss, s->spicfg);
  if(err)
    return err;
  cli_printf(cli, "ReadReg 0x891: %02x %02x %02x %02x %02x %02x %02x %02x\n",
             rd[0], rd[1], rd[2], rd[3], rd[4], rd[5], rd[6], rd[7]);

  // Does command processing work at all?
  uint8_t sb[2] = {SX1280_SET_STANDBY, SX1280_STDBY_RC};
  err = s->bus->rw(s->bus, sb, NULL, sizeof(sb), s->nss, s->spicfg);
  if(err)
    return err;
  usleep(10000);

  uint8_t st2[2] = {SX1280_GET_STATUS, 0};
  err = s->bus->rw(s->bus, st2, st2, sizeof(st2), s->nss, s->spicfg);
  if(err)
    return err;
  cli_printf(cli, "after SetStandby(RC): %02x %02x  BUSY:%d\n",
             st2[0], st2[1], gpio_get_input(s->busy));
  return 0;
}

// Phantom-power check: an unpowered module can run parasitically off
// the idle-high signal lines (NSS, NRESET) via its protection diodes.
// Removing the NSS feed makes such a chip brown out; a genuinely
// powered chip keeps driving BUSY high no matter what NSS does.
static error_t
cmd_sx1280_power(cli_t *cli)
{
  sx1280_t *s = sx1280_cli_instance;

  cli_printf(cli, "nss=1: BUSY:%s\n", pin_probe(s->busy));

  gpio_set_output(s->nss, 0);
  usleep(50000);
  cli_printf(cli, "nss=0: BUSY:%s\n", pin_probe(s->busy));

  gpio_set_output(s->nss, 1);
  gpio_conf_input(s->busy, GPIO_PULL_NONE);
  return 0;
}

static error_t
cmd_sx1280(cli_t *cli, int argc, char **argv)
{
  if(sx1280_cli_instance == NULL)
    return ERR_NO_DEVICE;

  // The bring-up commands (test/cw/pins/status/power) poke the radio
  // directly, behind the scheduler's back. That is tolerable while
  // idle or advertising (one garbled adv event self-heals) but would
  // corrupt a live connection's event timing.
  if(argc >= 2 && strcmp(argv[1], "ble")) {
    if(sx1280_ble_conn_active(sx1280_cli_instance)) {
      cli_printf(cli, "radio busy: BLE connection active, "
                 "'sx1280 ble drop' first\n");
      return ERR_NOT_READY;
    }
    // Invalidate the mode cache so the next slot does a full reconfig
    sx1280_sched_set_mode(sx1280_cli_instance, NULL);
  }

  if(argc >= 2 && !strcmp(argv[1], "test"))
    return cmd_sx1280_test(cli);

  if(argc >= 2 && !strcmp(argv[1], "cw")) {
    uint32_t mhz = argc >= 3 ? atoi(argv[2]) : 2440;
    return cmd_sx1280_cw(cli, mhz * 1000000u);
  }

  if(argc >= 2 && !strcmp(argv[1], "pins"))
    return cmd_sx1280_pins(cli);

  if(argc >= 2 && !strcmp(argv[1], "status"))
    return cmd_sx1280_status(cli);

  if(argc >= 2 && !strcmp(argv[1], "power"))
    return cmd_sx1280_power(cli);

  if(argc >= 2 && !strcmp(argv[1], "ble")) {
    sx1280_t *s = sx1280_cli_instance;
    if(argc >= 3 && !strcmp(argv[2], "adv")) {
      sx1280_ble_adv_start(s, argc >= 4 ? argv[3] : NULL, 0);
      cli_printf(cli, "advertising started\n");
    } else if(argc >= 3 && !strcmp(argv[2], "stop")) {
      sx1280_ble_adv_stop(s);
      cli_printf(cli, "advertising stopped\n");
    } else if(argc >= 3 && !strcmp(argv[2], "drop")) {
      sx1280_ble_conn_drop(s);
      cli_printf(cli, "terminating connection\n");
    } else if(argc >= 3 && !strcmp(argv[2], "sweep")) {
      sx1280_ble_adv_start(s, NULL, 1);
      cli_printf(cli, "whitening seed sweep started (ch37, ~40s/lap)\n");
    } else if(argc >= 4 && !strcmp(argv[2], "atx")) {
      sx1280_ble_set_autotx(s, atoi(argv[3]));
      cli_printf(cli, "autotx arm value: %d\n", atoi(argv[3]));
    } else if(argc >= 4 && !strcmp(argv[2], "pwr")) {
      sx1280_ble_set_txpower(s, atoi(argv[3]));
      cli_printf(cli, "tx power: %d dBm\n", atoi(argv[3]));
    } else if(argc >= 3) {
      cli_printf(cli, "usage: sx1280 ble "
                 "[adv [name]|stop|drop|sweep|atx <n>|pwr <dBm>]\n");
      return 0;
    }
    sx1280_ble_adv_report(s, cli->cl_stream);
    return 0;
  }

  cli_printf(cli, "usage: sx1280 test | cw [freq-mhz] | pins | status | power"
             " | ble [name|sweep|stop]\n");
  return 0;
}

CLI_CMD_DEF_EXT("sx1280", cmd_sx1280, "test | cw [freq-mhz]",
                "SX1280 radio bring-up");
