#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <mios/io.h>
#include <mios/mios.h>

#include "vgpio.h"
#include "vi2c.h"
#include "vtcal9539.h"

// See vtcal9539.h

// Register file index of each command byte (Table 8-3), pairs adjacent
enum {
  R_INPUT0, R_INPUT1,
  R_OUTPUT0, R_OUTPUT1,
  R_POL0, R_POL1,
  R_CFG0, R_CFG1,
  R_DRIVE0, R_DRIVE1, R_DRIVE2, R_DRIVE3,
  R_LATCH0, R_LATCH1,
  R_PE0, R_PE1,
  R_PUD0, R_PUD1,
  R_MASK0, R_MASK1,
  R_STATUS0, R_STATUS1,
  R_OUTCFG,
  R_NUM,
  R_NONE = R_NUM,
};

static int
command_to_reg(uint8_t cmd)
{
  if(cmd <= 0x07)
    return R_INPUT0 + cmd;
  if(cmd >= 0x40 && cmd <= 0x4d)
    return R_DRIVE0 + (cmd - 0x40);
  if(cmd == 0x4f)
    return R_OUTCFG;
  return R_NONE;
}

static const uint8_t reg_defaults[R_NUM] = {
  [R_OUTPUT0] = 0xff, [R_OUTPUT1] = 0xff,
  [R_CFG0] = 0xff, [R_CFG1] = 0xff,
  [R_DRIVE0] = 0xff, [R_DRIVE1] = 0xff, [R_DRIVE2] = 0xff, [R_DRIVE3] = 0xff,
  [R_PUD0] = 0xff, [R_PUD1] = 0xff,
  [R_MASK0] = 0xff, [R_MASK1] = 0xff,
};

typedef struct vtcal9539 {
  vi2c_dev_t dev;
  i2c_t *bus;
  gpio_t int_pin;

  uint8_t regs[R_NUM];
  uint8_t ptr;           // Register the next data byte goes to
  uint8_t expect_cmd;    // Next written byte is the command byte

  uint16_t ext_on;       // Lines driven from outside
  uint16_t ext;          // ... with these levels
  uint16_t ref;          // Levels the last input register read returned

  uint8_t int_asserted;
} vtcal9539_t;


static uint16_t
reg16(const vtcal9539_t *t, int r0)
{
  return t->regs[r0] | (t->regs[r0 + 1] << 8);
}


static uint16_t
levels(const vtcal9539_t *t)
{
  const uint16_t cfg = reg16(t, R_CFG0);  // 1 = input
  const uint16_t out = reg16(t, R_OUTPUT0);
  const uint16_t pe = reg16(t, R_PE0);
  const uint16_t pud = reg16(t, R_PUD0);

  uint16_t od = 0;
  if(t->regs[R_OUTCFG] & 1)
    od |= 0x00ff;
  if(t->regs[R_OUTCFG] & 2)
    od |= 0xff00;

  // Not driven by the chip: inputs, and open-drain outputs at 1
  const uint16_t released = cfg | (od & out);

  uint16_t outside = (pe & pud);               // Pulls (disabled on outputs)
  outside = (outside & ~t->ext_on) | (t->ext & t->ext_on);

  return (~released & out) | (released & outside);
}


static uint16_t
pending_irqs(const vtcal9539_t *t)
{
  const uint16_t cfg = reg16(t, R_CFG0);
  return (levels(t) ^ t->ref) & cfg;
}


static void
update_int(vtcal9539_t *t)
{
  const int asserted = !!(pending_irqs(t) & ~reg16(t, R_MASK0));
  if(asserted == t->int_asserted)
    return;
  t->int_asserted = asserted;
  if(t->int_pin == GPIO_UNUSED)
    return;
  if(asserted)
    vgpio_drive(t->int_pin, 0);
  else
    vgpio_release(t->int_pin);
}


static uint8_t
reg_read(vtcal9539_t *t, int r)
{
  switch(r) {
  case R_INPUT0:
  case R_INPUT1: {
    const uint16_t lv = levels(t);
    const uint16_t v = lv ^ (reg16(t, R_POL0) & reg16(t, R_CFG0));
    const uint16_t port = r == R_INPUT0 ? 0x00ff : 0xff00;
    t->ref = (t->ref & ~port) | (lv & port);  // Clears the port's IRQs
    return r == R_INPUT0 ? v : v >> 8;
  }
  case R_STATUS0:
  case R_STATUS1: {
    const uint16_t s = pending_irqs(t) & ~reg16(t, R_MASK0);
    return r == R_STATUS0 ? s : s >> 8;
  }
  case R_NONE:
    return 0;
  default:
    return t->regs[r];
  }
}


static void
reg_write(vtcal9539_t *t, int r, uint8_t v)
{
  switch(r) {
  case R_INPUT0:
  case R_INPUT1:
  case R_STATUS0:
  case R_STATUS1:
  case R_NONE:
    return;  // Read only
  case R_CFG0:
  case R_CFG1: {
    // Making a pin an output clears its interrupt
    const uint16_t was_input = reg16(t, R_CFG0);
    t->regs[r] = v;
    const uint16_t to_output = was_input & ~reg16(t, R_CFG0);
    t->ref = (t->ref & ~to_output) | (levels(t) & to_output);
    return;
  }
  default:
    t->regs[r] = v;
    return;
  }
}


static void
advance(vtcal9539_t *t)
{
  if(t->ptr < R_OUTCFG)
    t->ptr ^= 1;  // The other register of the pair
}


static int
tcal_start(vi2c_dev_t *d, uint8_t addr, int read)
{
  vtcal9539_t *t = (vtcal9539_t *)d;
  if(!read)
    t->expect_cmd = 1;
  return 0;
}


static int
tcal_write(vi2c_dev_t *d, uint8_t byte)
{
  vtcal9539_t *t = (vtcal9539_t *)d;
  if(t->expect_cmd) {
    t->expect_cmd = 0;
    t->ptr = command_to_reg(byte);
    return 0;
  }
  reg_write(t, t->ptr, byte);
  advance(t);
  update_int(t);
  return 0;
}


static uint8_t
tcal_read(vi2c_dev_t *d)
{
  vtcal9539_t *t = (vtcal9539_t *)d;
  uint8_t v = reg_read(t, t->ptr);
  advance(t);
  update_int(t);
  return v;
}


static void
tcal_stop(vi2c_dev_t *d)
{
}


static const vi2c_dev_ops_t vtcal9539_ops = {
  .start = tcal_start,
  .write = tcal_write,
  .read = tcal_read,
  .stop = tcal_stop,
};


static void
reset_locked(vtcal9539_t *t)
{
  memcpy(t->regs, reg_defaults, sizeof(t->regs));
  t->ptr = R_INPUT0;
  t->ref = levels(t);
  update_int(t);
}


struct vtcal9539 *
vtcal9539_create(i2c_t *bus, uint8_t addr, gpio_t int_pin)
{
  vtcal9539_t *t = calloc(1, sizeof(vtcal9539_t));
  t->dev.ops = &vtcal9539_ops;
  t->bus = bus;
  t->int_pin = int_pin;
  reset_locked(t);
  if(vi2c_attach(bus, addr, &t->dev))
    panic("vtcal9539: address 0x%x taken", addr);
  return t;
}


void
vtcal9539_drive(struct vtcal9539 *t, unsigned int line, int level)
{
  if(line > 15)
    return;
  vi2c_lock(t->bus, 1);
  t->ext_on |= 1 << line;
  if(level)
    t->ext |= 1 << line;
  else
    t->ext &= ~(1 << line);
  update_int(t);
  vi2c_lock(t->bus, 0);
}


void
vtcal9539_release(struct vtcal9539 *t, unsigned int line)
{
  if(line > 15)
    return;
  vi2c_lock(t->bus, 1);
  t->ext_on &= ~(1 << line);
  update_int(t);
  vi2c_lock(t->bus, 0);
}


int
vtcal9539_pin(struct vtcal9539 *t, unsigned int line)
{
  if(line > 15)
    return 0;
  vi2c_lock(t->bus, 1);
  int r = (levels(t) >> line) & 1;
  vi2c_lock(t->bus, 0);
  return r;
}


void
vtcal9539_reset(struct vtcal9539 *t)
{
  vi2c_lock(t->bus, 1);
  reset_locked(t);
  vi2c_lock(t->bus, 0);
}
