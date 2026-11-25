/*
 * "tcal9539": drivers/tcal9539.c against the datasheet model
 * (vtcal9539.c) on a virtual I2C bus, with and without an IRQ mux on
 * the INT line.
 */

#include <stdint.h>
#include <unistd.h>

#include <mios/io.h>
#include <drivers/tcal9539.h>

#include "hosttest.h"
#include "irq.h"
#include "vgpio.h"
#include "vi2c.h"
#include "vtcal9539.h"

#define INT_PIN GPIO_PA(0)

static int irq_hits;

static void
line_irq(void *arg)
{
  irq_hits++;
}


static int
test_polled(i2c_t *bus)
{
  int fails = 0;
  struct vtcal9539 *vt = vtcal9539_create(bus, 0x74, GPIO_UNUSED);

  fails += !CHECK(tcal9539_create(bus, 0x75, NULL) == NULL,
                  "no chip at 0x75: create fails");
  xgpio_t *xg = tcal9539_create(bus, 0x74, NULL);
  fails += !CHECK(xg != NULL, "create");
  if(xg == NULL)
    return fails;

  // Output with an initial value
  fails += !CHECK(xgpio_conf_output(xg, 3, GPIO_PUSH_PULL, GPIO_SPEED_LOW,
                                    GPIO_PULL_NONE, 0) == 0, "conf_output");
  fails += !CHECK(vtcal9539_pin(vt, 3) == 0, "line 3 driven low");
  xgpio_set(xg, 3, 1);
  fails += !CHECK(vtcal9539_pin(vt, 3) == 1, "line 3 driven high");

  // Input on the second port, with a pull-up
  fails += !CHECK(xgpio_conf_input(xg, 9, GPIO_PULL_UP) == 0, "conf_input");
  int v = -1;
  fails += !CHECK(xgpio_get(xg, 9, &v) == 0 && v == 1, "pulled up: %d", v);
  vtcal9539_drive(vt, 9, 0);
  fails += !CHECK(xgpio_get(xg, 9, &v) == 0 && v == 0, "driven low: %d", v);
  vtcal9539_release(vt, 9);
  fails += !CHECK(xgpio_get(xg, 9, &v) == 0 && v == 1, "released: %d", v);

  fails += !CHECK(xgpio_conf_input(xg, 10, GPIO_PULL_DOWN) == 0,
                  "conf_input");
  fails += !CHECK(vtcal9539_pin(vt, 10) == 0, "pulled down");

  // RESET_N pulsed while the MCU keeps running: the chip is back at its
  // defaults (all inputs, no pulls), the driver's shadows are not
  vtcal9539_reset(vt);
  fails += !CHECK(vtcal9539_pin(vt, 3) == 0, "after reset line 3 floats");
  fails += !CHECK(xgpio_get_mode(xg, 3) == 0,
                  "the driver still thinks line 3 is an output");
  uint8_t cfg1 = 0;
  i2c_read_u8(bus, 0x74, 0x07, &cfg1);
  fails += !CHECK(cfg1 == 0xff, "configuration 1 at default 0x%x", cfg1);

  // A second create reads the registers back, as a warm boot does
  xgpio_t *xg2 = tcal9539_create(bus, 0x74, NULL);
  fails += !CHECK(xg2 != NULL && xgpio_get_mode(xg2, 3) == 1,
                  "re-created driver sees line 3 as an input");
  return fails;
}


static int
test_irq(i2c_t *bus)
{
  int fails = 0;
  struct vtcal9539 *vt = vtcal9539_create(bus, 0x76, INT_PIN);
  struct xgpio_irq_mux *mux = xgpio_irq_mux_create(INT_PIN, IRQ_LEVEL_IO);
  xgpio_t *xg = tcal9539_create(bus, 0x76, mux);
  fails += !CHECK(xg != NULL, "create with IRQ mux");
  if(xg == NULL)
    return fails;

  fails += !CHECK(vgpio_level(INT_PIN) == 1, "INT idle high");

  fails += !CHECK(xgpio_conf_irq(xg, 2, GPIO_PULL_UP, line_irq, NULL,
                                 GPIO_FALLING_EDGE, IRQ_LEVEL_IO) == 0,
                  "conf_irq");
  usleep(10000);
  irq_hits = 0;

  vtcal9539_drive(vt, 2, 0);
  usleep(10000);
  fails += !CHECK(irq_hits == 1, "falling edge on line 2: %d calls",
                  irq_hits);
  fails += !CHECK(vgpio_level(INT_PIN) == 1,
                  "INT released once the driver read the inputs");

  int v = -1;
  fails += !CHECK(xgpio_get(xg, 2, &v) == 0 && v == 0,
                  "shadow updated: %d", v);

  vtcal9539_drive(vt, 2, 1);
  usleep(10000);
  fails += !CHECK(irq_hits == 1, "rising edge not asked for: %d calls",
                  irq_hits);
  fails += !CHECK(xgpio_get(xg, 2, &v) == 0 && v == 1,
                  "shadow updated on the rising edge too: %d", v);

  // A masked line changes nothing on INT (no pull: it sits at 0)
  vtcal9539_drive(vt, 5, 1);
  usleep(10000);
  fails += !CHECK(vgpio_level(INT_PIN) == 1, "masked line: INT stays high");
  fails += !CHECK(irq_hits == 1, "masked line: no call");
  return fails;
}


// Register-level checks of what the driver does not exercise: pair
// access, the interrupt rules, the mask and status registers
static int
test_raw(i2c_t *bus)
{
  int fails = 0;
  const gpio_t int_pin = GPIO_PA(1);
  gpio_conf_input(int_pin, GPIO_PULL_UP);  // As an MCU wires INT
  struct vtcal9539 *vt = vtcal9539_create(bus, 0x77, int_pin);

  // A write of two bytes after command 0x02 fills output 0 then 1, a read
  // from 0x03 returns output 1 then output 0
  const uint8_t wr[3] = {0x02, 0x12, 0x34};
  fails += !CHECK(i2c_rw(bus, 0x77, wr, 3, NULL, 0) == 0, "pair write");
  uint8_t cmd = 0x03;
  uint8_t rd[3] = {};
  fails += !CHECK(i2c_rw(bus, 0x77, &cmd, 1, rd, 3) == 0, "pair read");
  fails += !CHECK(rd[0] == 0x34 && rd[1] == 0x12 && rd[2] == 0x34,
                  "pair alternates: %02x %02x %02x", rd[0], rd[1], rd[2]);

  // Line 4 an input with a pull-up, then a read of input 0 so the
  // reference is the pulled-up level
  i2c_write_u8(bus, 0x77, 0x46, 0x10);   // Pull enable
  uint8_t v;
  i2c_read_u8(bus, 0x77, 0x00, &v);
  fails += !CHECK(v & 0x10, "pulled up input reads 1: 0x%02x", v);

  // Masked (the default): a change does not pull INT
  vtcal9539_drive(vt, 4, 0);
  fails += !CHECK(vgpio_level(int_pin) == 1, "masked change: INT high");
  i2c_read_u8(bus, 0x77, 0x4c, &v);
  fails += !CHECK(v == 0, "masked: status 0x%02x", v);

  // Unmask: the pending change pulls INT low
  i2c_write_u8(bus, 0x77, 0x4a, 0xef);
  fails += !CHECK(vgpio_level(int_pin) == 0, "unmasked change: INT low");
  i2c_read_u8(bus, 0x77, 0x4c, &v);
  fails += !CHECK(v == 0x10, "status names line 4: 0x%02x", v);

  // Reading input 1 does not clear a port 0 interrupt, input 0 does
  i2c_read_u8(bus, 0x77, 0x01, &v);
  fails += !CHECK(vgpio_level(int_pin) == 0, "input 1 read: INT still low");
  i2c_read_u8(bus, 0x77, 0x00, &v);
  fails += !CHECK(vgpio_level(int_pin) == 1, "input 0 read: INT released");
  fails += !CHECK(!(v & 0x10), "input 0 reads line 4 low: 0x%02x", v);

  // Back to the level last read before anyone reads: interrupt gone
  vtcal9539_release(vt, 4);
  fails += !CHECK(vgpio_level(int_pin) == 0, "release: INT low");
  vtcal9539_drive(vt, 4, 0);
  fails += !CHECK(vgpio_level(int_pin) == 1, "back to the read level: "
                  "INT released");

  // Polarity inversion applies to inputs
  i2c_write_u8(bus, 0x77, 0x04, 0x10);
  i2c_read_u8(bus, 0x77, 0x00, &v);
  fails += !CHECK(v & 0x10, "inverted input reads 1: 0x%02x", v);
  i2c_write_u8(bus, 0x77, 0x04, 0x00);

  // Making a pending input an output clears its interrupt
  vtcal9539_release(vt, 4);
  fails += !CHECK(vgpio_level(int_pin) == 0, "pending again: INT low");
  i2c_write_u8(bus, 0x77, 0x06, 0xef);
  fails += !CHECK(vgpio_level(int_pin) == 1, "made an output: INT released");
  i2c_write_u8(bus, 0x77, 0x06, 0xff);
  fails += !CHECK(vgpio_level(int_pin) == 1, "an input again at the level "
                  "it drove: no interrupt");

  // Open drain port 0: an output at 1 lets go, the pull decides
  i2c_write_u8(bus, 0x77, 0x02, 0x10);
  i2c_write_u8(bus, 0x77, 0x06, 0xef);
  i2c_write_u8(bus, 0x77, 0x4f, 0x01);
  vtcal9539_drive(vt, 4, 0);
  fails += !CHECK(vtcal9539_pin(vt, 4) == 0, "open drain 1 pulled low "
                  "from outside");
  i2c_write_u8(bus, 0x77, 0x4f, 0x00);
  fails += !CHECK(vtcal9539_pin(vt, 4) == 1, "push-pull 1 drives high");
  return fails;
}


static int
suite_tcal9539(void)
{
  int fails = 0;
  i2c_t *bus = vi2c_create("vi2c");
  fails += test_polled(bus);
  fails += test_irq(bus);
  fails += test_raw(bus);
  return fails;
}

HOSTTEST_SUITE("tcal9539", suite_tcal9539, 0);
