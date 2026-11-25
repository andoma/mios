/*
 * "vbus": the host's GPIO pins (vgpio.c) and the virtual I2C and SPI
 * buses (vi2c.c, vspi.c) that device models attach to.
 *
 * Pins are checked for the STM32 behaviours board code relies on (the
 * output latch, reading back what an output drives, open drain, edge
 * IRQs), the buses with small models written here (an I2C register file
 * that can NACK, an SPI chip that echoes), and the generic register-file
 * model (vregfile.c) on both.
 */

#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/uio.h>

#include <mios/io.h>
#include <mios/task.h>

#include "hosttest.h"
#include "irq.h"
#include "vgpio.h"
#include "vi2c.h"
#include "vspi.h"
#include "vregfile.h"

// ---- GPIO

static int irq_count;

static void
count_irq(void *arg)
{
  irq_count++;
}


static int
test_gpio(void)
{
  int fails = 0;
  const gpio_t p = GPIO_PB(3);

  fails += !CHECK(vgpio_level(p) == 0, "floating input reads 0");
  gpio_conf_input(p, GPIO_PULL_UP);
  fails += !CHECK(gpio_get_input(p) == 1, "pull-up reads 1");

  // The latch is set while still an input, the pin drives it once it is
  // an output (illumination.c configures its chip selects this way)
  gpio_set_output(p, 0);
  fails += !CHECK(gpio_get_input(p) == 1, "latch drives nothing as input");
  fails += !CHECK(vgpio_driven(p) == -1, "input does not drive");
  gpio_conf_output(p, GPIO_PUSH_PULL, GPIO_SPEED_LOW, GPIO_PULL_NONE);
  fails += !CHECK(vgpio_driven(p) == 0, "output drives the latch");
  fails += !CHECK(gpio_get_input(p) == 0, "output reads back its level");
  gpio_set_output(p, 1);
  fails += !CHECK(vgpio_level(p) == 1, "output at 1");

  // Push-pull wins over something driving from outside
  vgpio_drive(p, 0);
  fails += !CHECK(vgpio_level(p) == 1, "push-pull 1 overrides outside 0");
  vgpio_release(p);

  // Open drain at 1 lets go: the outside or the pull decides
  const gpio_t od = GPIO_PB(4);
  gpio_conf_output(od, GPIO_OPEN_DRAIN, GPIO_SPEED_LOW, GPIO_PULL_UP);
  gpio_set_output(od, 1);
  fails += !CHECK(vgpio_driven(od) == -1, "open drain 1 does not drive");
  fails += !CHECK(vgpio_level(od) == 1, "open drain 1 pulled up");
  vgpio_drive(od, 0);
  fails += !CHECK(vgpio_level(od) == 0, "open drain 1 pulled low outside");
  vgpio_release(od);
  gpio_set_output(od, 0);
  fails += !CHECK(vgpio_level(od) == 0, "open drain 0 drives low");

  // Edge IRQs
  const gpio_t ip = GPIO_PC(5);
  irq_count = 0;
  gpio_conf_irq(ip, GPIO_PULL_UP, count_irq, NULL, GPIO_FALLING_EDGE,
                IRQ_LEVEL_IO);
  usleep(1000);
  fails += !CHECK(irq_count == 0, "configuring is not an edge");
  vgpio_drive(ip, 0);
  usleep(1000);
  fails += !CHECK(irq_count == 1, "falling edge: %d IRQs, want 1",
                  irq_count);
  vgpio_drive(ip, 1);
  usleep(1000);
  fails += !CHECK(irq_count == 1, "rising edge ignored: %d IRQs",
                  irq_count);
  gpio_conf_irq_edge(ip, GPIO_BOTH_EDGES);
  vgpio_drive(ip, 0);
  vgpio_release(ip);  // Back to the pull-up: a rising edge
  usleep(1000);
  fails += !CHECK(irq_count == 3, "both edges: %d IRQs, want 3", irq_count);
  gpio_disconnect(ip);
  vgpio_drive(ip, 0);
  usleep(1000);
  fails += !CHECK(irq_count == 3, "disconnected pin fires no IRQ");
  vgpio_release(ip);

  fails += !CHECK(vgpio_level(-1) == 0 && vgpio_level(HOST_GPIO_COUNT) == 0,
                  "pins out of range read 0");
  return fails;
}


// ---- I2C: a 256 byte register file, pointer set by the first written byte

typedef struct regfile {
  vi2c_dev_t dev;
  uint8_t regs[256];
  uint8_t ptr;
  uint8_t first;
  int nack_data;     // NACK data bytes
  int starts, stops;
} regfile_t;

static int
rf_start(vi2c_dev_t *d, uint8_t addr, int read)
{
  regfile_t *rf = (regfile_t *)d;
  rf->starts++;
  if(!read)
    rf->first = 1;
  return 0;
}

static int
rf_write(vi2c_dev_t *d, uint8_t b)
{
  regfile_t *rf = (regfile_t *)d;
  if(rf->first) {
    rf->first = 0;
    rf->ptr = b;
    return 0;
  }
  if(rf->nack_data)
    return 1;
  rf->regs[rf->ptr++] = b;
  return 0;
}

static uint8_t
rf_read(vi2c_dev_t *d)
{
  regfile_t *rf = (regfile_t *)d;
  return rf->regs[rf->ptr++];
}

static void
rf_stop(vi2c_dev_t *d)
{
  ((regfile_t *)d)->stops++;
}

static const vi2c_dev_ops_t rf_ops = {
  .start = rf_start, .write = rf_write, .read = rf_read, .stop = rf_stop,
};


static int
test_i2c(void)
{
  int fails = 0;
  i2c_t *bus = vi2c_create("vi2c");
  static regfile_t rf = { .dev = { &rf_ops } };

  uint8_t v = 0;
  fails += !CHECK(i2c_read_u8(bus, 0x20, 0, &v) == ERR_NO_DEVICE,
                  "nothing at the address: ERR_NO_DEVICE");

  fails += !CHECK(vi2c_attach(bus, 0x20, &rf.dev) == 0, "attach");
  fails += !CHECK(vi2c_attach(bus, 0x20, &rf.dev) == ERR_EXIST,
                  "attach twice: ERR_EXIST");

  fails += !CHECK(i2c_write_u8(bus, 0x20, 0x10, 0xab) == 0, "write");
  fails += !CHECK(rf.regs[0x10] == 0xab, "the byte reached the model");
  fails += !CHECK(i2c_read_u8(bus, 0x20, 0x10, &v) == 0 && v == 0xab,
                  "read back 0x%x", v);
  fails += !CHECK(rf.starts == 3 && rf.stops == 2,
                  "write, then write + repeated start read: %d starts, "
                  "%d stops", rf.starts, rf.stops);

  // Gathered write: the segments are one write phase
  const uint8_t hdr[2] = {0x30, 1};
  const uint8_t data[2] = {2, 3};
  const struct iovec tx[2] = {{(void *)hdr, 2}, {(void *)data, 2}};
  fails += !CHECK(bus->rwv(bus, 0x20, tx, NULL, 2) == 0, "gathered write");
  fails += !CHECK(rf.regs[0x30] == 1 && rf.regs[0x31] == 2 &&
                  rf.regs[0x32] == 3, "gathered bytes in order");

  rf.nack_data = 1;
  fails += !CHECK(i2c_write_u8(bus, 0x20, 0x10, 1) == ERR_BUS_ERROR,
                  "data NACK: ERR_BUS_ERROR");
  rf.nack_data = 0;

  vi2c_set_error(bus, 0x20, ERR_TIMEOUT);
  const int starts = rf.starts;
  fails += !CHECK(i2c_read_u8(bus, 0x20, 0, &v) == ERR_TIMEOUT,
                  "forced error");
  fails += !CHECK(rf.starts == starts, "forced error does not reach the model");
  vi2c_set_error(bus, 0x20, 0);

  vi2c_detach(bus, 0x20);
  fails += !CHECK(i2c_read_u8(bus, 0x20, 0, &v) == ERR_NO_DEVICE,
                  "detached: ERR_NO_DEVICE");
  return fails;
}


// ---- SPI: a chip that shifts out the previous byte it received

typedef struct echo {
  vspi_dev_t dev;
  uint8_t prev;
  int selects;
  int mode;
} echo_t;

static void
echo_select(vspi_dev_t *d, int mode)
{
  echo_t *e = (echo_t *)d;
  e->selects++;
  e->mode = mode;
  e->prev = 0x00;
}

static uint8_t
echo_xfer(vspi_dev_t *d, uint8_t mosi)
{
  echo_t *e = (echo_t *)d;
  uint8_t r = e->prev;
  e->prev = mosi;
  return r;
}

static void
echo_deselect(vspi_dev_t *d)
{
}

static const vspi_dev_ops_t echo_ops = {
  .select = echo_select, .xfer = echo_xfer, .deselect = echo_deselect,
};


static int
test_spi(void)
{
  int fails = 0;
  spi_t *bus = vspi_create("vspi");
  static echo_t a = { .dev = { &echo_ops } };
  static echo_t b = { .dev = { &echo_ops } };
  const gpio_t cs_a = GPIO_PE(10);
  const gpio_t cs_b = GPIO_PE(12);

  fails += !CHECK(vspi_attach(bus, cs_a, &a.dev) == 0, "attach a");
  fails += !CHECK(vspi_attach(bus, cs_b, &b.dev) == 0, "attach b");
  fails += !CHECK(vspi_attach(bus, cs_b, &a.dev) == ERR_EXIST,
                  "two chips on one chip select: ERR_EXIST");

  // cs_a is never made an output: driving it low does nothing, the chip
  // is not selected and nobody drives MISO
  const uint8_t tx[3] = {0x11, 0x22, 0x33};
  uint8_t rx[3];
  const int cfg = bus->get_config(bus, SPI_CPHA, 1000000);
  fails += !CHECK(bus->rw(bus, tx, rx, 3, cs_a, cfg) == 0, "rw");
  fails += !CHECK(a.selects == 0, "chip select not an output: not selected");
  fails += !CHECK(rx[0] == 0xff && rx[1] == 0xff && rx[2] == 0xff,
                  "MISO undriven reads 0xff");

  gpio_set_output(cs_a, 1);
  gpio_conf_output(cs_a, GPIO_PUSH_PULL, GPIO_SPEED_HIGH, GPIO_PULL_NONE);
  gpio_set_output(cs_b, 1);
  gpio_conf_output(cs_b, GPIO_PUSH_PULL, GPIO_SPEED_HIGH, GPIO_PULL_NONE);

  fails += !CHECK(bus->rw(bus, tx, rx, 3, cs_a, cfg) == 0, "rw");
  fails += !CHECK(a.selects == 1 && b.selects == 0, "a selected, b not");
  fails += !CHECK(a.mode == SPI_CPHA, "mode from get_config: %d", a.mode);
  fails += !CHECK(rx[0] == 0x00 && rx[1] == 0x11 && rx[2] == 0x22,
                  "echoed %02x %02x %02x", rx[0], rx[1], rx[2]);
  fails += !CHECK(vgpio_level(cs_a) == 1, "chip select high again");

  // A NULL rx is a write
  fails += !CHECK(bus->rw(bus, tx, NULL, 3, cs_b, 0) == 0, "write only");
  fails += !CHECK(b.selects == 1 && b.prev == 0x33, "b got the bytes");

  // Scatter-gather is one transfer
  const struct iovec txv[2] = {{(void *)tx, 1}, {(void *)(tx + 1), 2}};
  uint8_t r0[1], r1[2];
  const struct iovec rxv[2] = {{r0, 1}, {r1, 2}};
  fails += !CHECK(bus->rwv(bus, txv, rxv, 2, cs_b, 0) == 0, "rwv");
  fails += !CHECK(b.selects == 2, "rwv selects once");
  fails += !CHECK(r0[0] == 0x00 && r1[0] == 0x11 && r1[1] == 0x22,
                  "rwv bytes %02x %02x %02x", r0[0], r1[0], r1[1]);

  // Both selected (a board fault): open-drain MISO ANDs them
  gpio_set_output(cs_b, 0);
  fails += !CHECK(bus->rw(bus, tx, rx, 2, cs_a, 0) == 0, "rw");
  fails += !CHECK(rx[1] == 0x11, "wired-AND of two echoes");
  gpio_set_output(cs_b, 1);

  vspi_detach(bus, cs_a);
  const int sel = a.selects;
  bus->rw(bus, tx, rx, 1, cs_a, 0);
  fails += !CHECK(a.selects == sel && rx[0] == 0xff, "detached");
  return fails;
}


// ---- The register-file model on both buses

static int
test_regfile(void)
{
  int fails = 0;

  i2c_t *i2c = vi2c_create("vi2c");
  static vregfile_t ri;
  fails += !CHECK(vregfile_attach_i2c(&ri, i2c, 0x48) == 0, "attach i2c");
  ri.regs[0x00] = 0x12;
  ri.regs[0x01] = 0x34;
  uint8_t b[2] = {};
  fails += !CHECK(i2c_read_bytes(i2c, 0x48, 0x00, b, 2) == 0 &&
                  b[0] == 0x12 && b[1] == 0x34, "i2c read %02x %02x",
                  b[0], b[1]);
  // The pointer stays after the transfer: a bare read continues at 0x02
  ri.regs[0x02] = 0x56;
  fails += !CHECK(i2c_rw(i2c, 0x48, NULL, 0, b, 1) == 0 && b[0] == 0x56,
                  "pointer kept between transfers: %02x", b[0]);
  const uint8_t w[3] = {0xff, 0xaa, 0xbb};
  fails += !CHECK(i2c_rw(i2c, 0x48, w, 3, NULL, 0) == 0, "i2c write");
  fails += !CHECK(ri.regs[0xff] == 0xaa && ri.regs[0x00] == 0xbb,
                  "write wraps at 256");

  spi_t *spi = vspi_create("vspi");
  static vregfile_t rs;
  const gpio_t cs = GPIO_PB(12);
  gpio_set_output(cs, 1);
  gpio_conf_output(cs, GPIO_PUSH_PULL, GPIO_SPEED_HIGH, GPIO_PULL_NONE);
  fails += !CHECK(vregfile_attach_spi(&rs, spi, cs) == 0, "attach spi");

  // Write two registers from 0x10, read them back with bit 7 set, tx and
  // rx in one buffer as drivers often do
  uint8_t buf[3] = {0x10, 0x01, 0x02};
  spi->rw(spi, buf, buf, 3, cs, 0);
  fails += !CHECK(buf[0] == 0 && buf[1] == 0 && buf[2] == 0,
                  "MISO 0 during a write");
  fails += !CHECK(rs.regs[0x10] == 0x01 && rs.regs[0x11] == 0x02,
                  "spi write");
  uint8_t rd[3] = {0x90, 0, 0};
  spi->rw(spi, rd, rd, 3, cs, 0);
  fails += !CHECK(rd[0] == 0 && rd[1] == 0x01 && rd[2] == 0x02,
                  "spi read %02x %02x %02x", rd[0], rd[1], rd[2]);
  return fails;
}


static int
suite_vbus(void)
{
  int fails = 0;
  fails += test_gpio();
  fails += test_i2c();
  fails += test_spi();
  fails += test_regfile();
  return fails;
}

HOSTTEST_SUITE("vbus", suite_vbus, 0);
