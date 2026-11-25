/*
 * "tps92682": drivers/tps92682.c against the datasheet model
 * (vtps92682.c) on a virtual SPI bus.
 *
 * The driver steps follow how a board runs it (lynxpdu's illumination
 * loop): read FLT1 to clear the power-cycled bit, init, set currents,
 * poll FLT1/FLT2. The raw tests send frames by hand to check the
 * protocol rules the driver does not exercise (parity, the frame after
 * power-up, the write response).
 */

#include <stdint.h>
#include <string.h>

#include <mios/io.h>
#include <drivers/tps92682.h>

#include "hosttest.h"
#include "vspi.h"
#include "vtps92682.h"

#define CS  GPIO_PD(14)
#define CS2 GPIO_PD(15)

static int listener_calls;

static void
listener(void *opaque, struct vtps92682 *t, unsigned int ch)
{
  listener_calls++;
}


static void
conf_cs(gpio_t cs)
{
  // As lynxpdu's illumination_gpio_init()
  gpio_conf_input(cs, GPIO_PULL_UP);
  gpio_set_output(cs, 1);
  gpio_conf_output(cs, GPIO_PUSH_PULL, GPIO_SPEED_VERY_HIGH, GPIO_PULL_NONE);
}


// w with the parity bit set right (odd parity over the frame)
static uint16_t
par(uint16_t w)
{
  w &= ~0x100;
  if(!(__builtin_popcount(w) & 1))
    w |= 0x100;
  return w;
}

// One raw 16-bit frame, returns what came back
static uint16_t
frame(spi_t *bus, uint16_t w, int fix_parity)
{
  if(fix_parity)
    w = par(w);
  uint8_t tx[2] = {w >> 8, w};
  uint8_t rx[2];
  bus->rw(bus, tx, rx, 2, CS, 0);
  return (rx[0] << 8) | rx[1];
}

#define WR(addr, data) (0x8000 | ((addr) << 9) | (data))
#define RD(addr)       ((addr) << 9)


static int
test_driver(spi_t *bus, struct vtps92682 *vt)
{
  int fails = 0;
  struct tps92682 *t = tps92682_create(bus, CS);

  // Straight after power-up PC blocks the channel enables, so init and
  // a current setting light nothing
  fails += !CHECK(tps92682_init(t, 0) == 0, "init");
  tps92682_set_ilimit_raw(t, 0, 100);
  fails += !CHECK(!vtps92682_output(vt, 0).on, "PC set: channel stays off");

  int flt1 = tps92682_read_reg(t, TPS92682_FLT1);
  fails += !CHECK(flt1 == TPS92682_FLT1_PC, "FLT1 after power-up: 0x%x",
                  flt1);
  flt1 = tps92682_read_reg(t, TPS92682_FLT1);
  fails += !CHECK(flt1 == 0, "PC cleared by the read: FLT1 0x%x", flt1);
  fails += !CHECK(!vtps92682_output(vt, 0).on,
                  "the enable written while PC was set did not stick");

  // The board's order: FLT1 read first, then init
  fails += !CHECK(tps92682_init(t, 0) == 0, "init");
  tps92682_set_ilimit_raw(t, 0, 100);
  tps92682_set_ilimit_raw(t, 1, 5);
  vtps92682_output_t o0 = vtps92682_output(vt, 0);
  vtps92682_output_t o1 = vtps92682_output(vt, 1);
  fails += !CHECK(o0.on && o0.iadj == 100 && o0.pwm == 0x3ff,
                  "ch1 on:%d iadj:%d pwm:%d", o0.on, o0.iadj, o0.pwm);
  fails += !CHECK(o1.on && o1.iadj == 5, "ch2 on:%d iadj:%d", o1.on, o1.iadj);
  fails += !CHECK(listener_calls > 0, "listener told of the changes");
  fails += !CHECK(vtps92682_peek(vt, TPS92682_FEN1) == 0x3d,
                  "UV enabled on ch1 only (iadj > 10): FEN1 0x%x",
                  vtps92682_peek(vt, TPS92682_FEN1));

  // UV on ch1: init latches UV (FLATEN = 3), so the channel goes dark
  // and stays dark when the fault is gone
  vtps92682_set_fault(vt, 0, VTPS92682_FAULT_UV, 1);
  fails += !CHECK(!vtps92682_output(vt, 0).on, "UV turns ch1 off");
  vtps92682_set_fault(vt, 0, VTPS92682_FAULT_UV, 0);
  fails += !CHECK(!vtps92682_output(vt, 0).on, "latched: still off");
  flt1 = tps92682_read_reg(t, TPS92682_FLT1);
  fails += !CHECK(flt1 == TPS92682_FLT1_CH1UV, "FLT1 shows CH1UV: 0x%x",
                  flt1);
  flt1 = tps92682_read_reg(t, TPS92682_FLT1);
  fails += !CHECK(flt1 == 0, "fault gone, read cleared it: 0x%x", flt1);

  // The driver writes EN only when its cached copy changes, so the same
  // level again does not re-arm the channel; going through 0 does
  tps92682_set_ilimit_raw(t, 0, 100);
  fails += !CHECK(!vtps92682_output(vt, 0).on,
                  "same level: EN not rewritten, still off");
  tps92682_set_ilimit_raw(t, 0, 0);
  tps92682_set_ilimit_raw(t, 0, 100);
  fails += !CHECK(vtps92682_output(vt, 0).on, "via 0: re-armed");

  // UV on ch2, where the driver disabled UV (iadj < 10): no fault
  vtps92682_set_fault(vt, 1, VTPS92682_FAULT_UV, 1);
  flt1 = tps92682_read_reg(t, TPS92682_FLT1);
  fails += !CHECK(flt1 == 0 && vtps92682_output(vt, 1).on,
                  "disabled UV: FLT1 0x%x, ch2 on:%d", flt1,
                  vtps92682_output(vt, 1).on);
  vtps92682_set_fault(vt, 1, VTPS92682_FAULT_UV, 0);

  // Undercurrent (an open string): a diagnostic, the channel stays on
  vtps92682_set_fault(vt, 0, VTPS92682_FAULT_UC, 1);
  int flt2 = tps92682_read_reg(t, TPS92682_FLT2);
  fails += !CHECK(flt2 == 0x40, "FLT2 CH1UC: 0x%x", flt2);
  fails += !CHECK(vtps92682_output(vt, 0).on, "UC does not turn it off");
  flt2 = tps92682_read_reg(t, TPS92682_FLT2);
  fails += !CHECK(flt2 == 0x40, "UC still there, not cleared: 0x%x", flt2);
  vtps92682_set_fault(vt, 0, VTPS92682_FAULT_UC, 0);
  tps92682_read_reg(t, TPS92682_FLT2);
  flt2 = tps92682_read_reg(t, TPS92682_FLT2);
  fails += !CHECK(flt2 == 0, "UC gone and read: 0x%x", flt2);

  // Thermal warning
  vtps92682_set_fault(vt, 0, VTPS92682_FAULT_TW, 1);
  flt1 = tps92682_read_reg(t, TPS92682_FLT1);
  fails += !CHECK(flt1 & TPS92682_FLT1_TW, "FLT1 TW: 0x%x", flt1);
  vtps92682_set_fault(vt, 0, VTPS92682_FAULT_TW, 0);

  // Spread spectrum
  tps92682_set_spread_specturm(t, 2);
  fails += !CHECK(vtps92682_peek(vt, TPS92682_FM) == 0x2d, "FM 0x%x",
                  vtps92682_peek(vt, TPS92682_FM));

  // A power cycle: everything off, PC up, the driver sees it in FLT1 (the
  // board's cue to init again)
  vtps92682_power_cycle(vt);
  fails += !CHECK(!vtps92682_output(vt, 0).on && !vtps92682_output(vt, 1).on,
                  "power cycle: outputs off");
  flt1 = tps92682_read_reg(t, TPS92682_FLT1);
  fails += !CHECK(flt1 & TPS92682_FLT1_PC, "PC after power cycle: 0x%x",
                  flt1);

  // Dead chip
  vtps92682_set_dead(vt, 1);
  fails += !CHECK(tps92682_read_reg(t, TPS92682_EN) == ERR_NO_DEVICE,
                  "dead chip: ERR_NO_DEVICE");
  fails += !CHECK(tps92682_init(t, 0) == ERR_NO_DEVICE, "dead chip: init");
  vtps92682_set_dead(vt, 0);

  // A chip whose chip select was never made an output does not answer
  struct tps92682 *t2 = tps92682_create(bus, CS2);
  fails += !CHECK(tps92682_read_reg(t2, TPS92682_EN) == ERR_NO_DEVICE,
                  "unconfigured chip select: ERR_NO_DEVICE");

  fails += !CHECK(vtps92682_spi_errors(vt) == 0,
                  "the driver caused %d SPI errors",
                  vtps92682_spi_errors(vt));
  return fails;
}


static int
test_frames(spi_t *bus, struct vtps92682 *vt)
{
  int fails = 0;
  vtps92682_power_cycle(vt);

  uint16_t r = frame(bus, RD(0x00), 1);
  fails += !CHECK(r == 0x8000, "first frame after power-up: 0x%04x", r);

  r = frame(bus, RD(0x04), 1);
  fails += !CHECK(r == (0x6000 | 0x0200 | 0x3c),
                  "read response EN, PC set: 0x%04x", r);

  r = frame(bus, WR(0x07, 0x5a), 1);
  fails += !CHECK((r & 0xff) == 0x55, "read response ISLOPE: 0x%04x", r);

  r = frame(bus, WR(0x08, 0x66), 1);
  fails += !CHECK(r == (0x4000 | (0x07 << 8) | 0x5a),
                  "write response echoes the write: 0x%04x", r);

  // Bad parity: SPE, and the write does not happen
  frame(bus, par(WR(0x07, 0x11)) ^ 0x100, 0);
  r = frame(bus, RD(0x07), 1);
  fails += !CHECK(r == 0x8000, "after a bad-parity write: 0x%04x", r);
  r = frame(bus, RD(0x07), 1);
  fails += !CHECK(r == (0x6200 | 0x5a), "register unchanged: 0x%04x", r);

  // A read with data bits set is a SPI error too
  frame(bus, RD(0x07) | 0x01, 1);
  r = frame(bus, RD(0x00), 1);
  fails += !CHECK(r & 0x8000, "read with data bits: SPE in 0x%04x", r);

  // 8 clocks: SPE
  uint8_t one = 0;
  uint8_t rx[2];
  bus->rw(bus, &one, rx, 1, CS, 0);
  uint8_t tx[2] = {RD(0x00) >> 8, 0};
  tx[0] |= 0x01;  // Parity of an all-zero read
  bus->rw(bus, tx, rx, 2, CS, 0);
  fails += !CHECK(rx[0] & 0x80, "short frame: SPE, got 0x%02x%02x",
                  rx[0], rx[1]);

  // RESET restores registers, not PC
  frame(bus, WR(0x26, 0xc3), 1);
  frame(bus, RD(0x07), 1);
  r = frame(bus, RD(0x11), 1);
  fails += !CHECK(r == 0x6200, "after RESET: CH1IADJ 0, PC still set: "
                  "0x%04x", r);
  r = frame(bus, RD(0x11), 1);
  fails += !CHECK((r & 0xff) == 0x20, "FLT1 shows PC: 0x%04x", r);
  r = frame(bus, RD(0x00), 1);
  fails += !CHECK((r & 0x0200) == 0, "PC cleared at the end of the "
                  "response: 0x%04x", r);

  // Unlisted registers read 0
  frame(bus, RD(0x30), 1);
  r = frame(bus, RD(0x00), 1);
  fails += !CHECK((r & 0xff) == 0, "unlisted register: 0x%04x", r);
  return fails;
}


static int
suite_tps92682(void)
{
  int fails = 0;
  spi_t *bus = vspi_create("vspi");
  conf_cs(CS);
  struct vtps92682 *vt = vtps92682_create(bus, CS);
  vtps92682_set_listener(vt, listener, NULL);

  fails += test_driver(bus, vt);
  fails += test_frames(bus, vt);
  return fails;
}

HOSTTEST_SUITE("tps92682", suite_tps92682, 0);
