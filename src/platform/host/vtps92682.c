#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <mios/io.h>
#include <mios/mios.h>

#include "vspi.h"
#include "vtps92682.h"

// See vtps92682.h

#define REG_EN      0x00
#define REG_CFG1    0x01
#define REG_CH1IADJ 0x07
#define REG_CH1PWML 0x0a
#define REG_FLT1    0x11
#define REG_FLT2    0x12
#define REG_FEN1    0x13
#define REG_FEN2    0x14
#define REG_FLATEN  0x15
#define REG_RESET   0x26
#define REG_LAST    0x26

#define EN_FPINRST  0x80
#define CFG1_INTPWM 0x40

#define FLT1_RTO    0x80
#define FLT1_PC     0x20
#define FLT1_TW     0x10

#define FRAME_POR   0x8000

// Table 7-2
static const uint8_t reg_defaults[REG_LAST + 1] = {
  [0x00] = 0x3c, [0x04] = 0x55, [0x05] = 0x05, [0x06] = 0x77,
  [0x09] = 0x01, [0x0e] = 0x0f, [0x0f] = 0x0a, [0x10] = 0x99,
  [0x13] = 0x3c, [0x14] = 0x0f, [0x16] = 0x22, [0x17] = 0x3c,
  [0x1e] = 0x0f, [0x1f] = 0x0a, [0x20] = 0x99, [0x21] = 0x3c,
  [0x22] = 0x0f, [0x24] = 0x22,
};

typedef struct vtps92682 {
  vspi_dev_t dev;
  spi_t *bus;

  uint8_t regs[REG_LAST + 1];

  uint8_t pc;
  uint8_t flt1;            // Fault read bits, without PC
  uint8_t flt2;
  uint8_t latched[2];      // A latched fault holds the channel off
  uint8_t faults[2];       // Fault conditions set by the test
  uint8_t shared;          // TW / RTO conditions

  uint8_t dead;

  // The frame in progress
  uint16_t out;            // What we shift out
  uint16_t last16;         // The last 16 bits received
  unsigned int nbytes;
  uint8_t mode;

  uint16_t next_out;       // The answer to the last command
  uint8_t clear_after;     // FLT register to clear at the end of this frame

  vtps92682_output_t outputs[2];

  unsigned int spi_errors;

  void (*listener)(void *opaque, struct vtps92682 *t, unsigned int ch);
  void *listener_opaque;
} vtps92682_t;


// FEN1/FEN2/FLATEN bit for a fault on a channel, 0 if it has none.
// See Table 7-1, 7-22, 7-23, 7-24.
static int
fault_enabled(const vtps92682_t *t, unsigned int ch, uint8_t f)
{
  const uint8_t fen1 = t->regs[REG_FEN1];
  const uint8_t fen2 = t->regs[REG_FEN2];
  switch(f) {
  case VTPS92682_FAULT_UV:   return fen1 & (0x01 << ch);
  case VTPS92682_FAULT_OV:   return fen1 & (0x04 << ch);
  case VTPS92682_FAULT_ISO:  return fen2 & (0x01 << ch);
  case VTPS92682_FAULT_ILIM: return fen2 & (0x04 << ch);
  case VTPS92682_FAULT_OC:   return fen2 & (0x10 << ch);
  case VTPS92682_FAULT_UC:   return 1;  // Diagnostic, always on
  default:                   return 0;
  }
}

static int
fault_latching(const vtps92682_t *t, unsigned int ch, uint8_t f)
{
  const uint8_t fl = t->regs[REG_FLATEN];
  switch(f) {
  case VTPS92682_FAULT_UV:   return fl & (0x01 << ch);
  case VTPS92682_FAULT_OV:   return fl & (0x04 << ch);
  case VTPS92682_FAULT_OC:   return fl & (0x10 << ch);
  case VTPS92682_FAULT_ILIM: return fl & (0x40 << ch);
  default:                   return 0;
  }
}

// Faults that turn the channel off (Table 7-1: UC is a diagnostic)
static int
fault_disables(uint8_t f)
{
  return f != VTPS92682_FAULT_UC;
}

static void
fault_read_bit(uint8_t *flt1, uint8_t *flt2, unsigned int ch, uint8_t f)
{
  switch(f) {
  case VTPS92682_FAULT_UV:   *flt1 |= 0x01 << ch; break;
  case VTPS92682_FAULT_OV:   *flt1 |= 0x04 << ch; break;
  case VTPS92682_FAULT_ISO:  *flt2 |= 0x01 << ch; break;
  case VTPS92682_FAULT_ILIM: *flt2 |= 0x04 << ch; break;
  case VTPS92682_FAULT_OC:   *flt2 |= 0x10 << ch; break;
  case VTPS92682_FAULT_UC:   *flt2 |= 0x40 << ch; break;
  }
}

static int
channel_enabled(const vtps92682_t *t, unsigned int ch)
{
  return (t->regs[REG_EN] & (1 << ch)) && !t->pc;
}

// The FLT read bits of the faults that exist now
static void
active_bits(const vtps92682_t *t, uint8_t *flt1, uint8_t *flt2)
{
  *flt1 = 0;
  *flt2 = 0;
  if(t->shared & VTPS92682_FAULT_TW)
    *flt1 |= FLT1_TW;
  if(t->shared & VTPS92682_FAULT_RTO)
    *flt1 |= FLT1_RTO;

  for(unsigned int ch = 0; ch < 2; ch++) {
    for(uint8_t f = 1; f <= VTPS92682_FAULT_ISO; f <<= 1) {
      if(!(t->faults[ch] & f) || !fault_enabled(t, ch, f))
        continue;
      if(f != VTPS92682_FAULT_ISO && !channel_enabled(t, ch))
        continue;
      fault_read_bit(flt1, flt2, ch, f);
    }
  }
}


// Re-evaluate faults and outputs after anything changed
static void
update(vtps92682_t *t)
{
  int hold_off[2] = {0, 0};

  for(unsigned int ch = 0; ch < 2; ch++) {
    if(!channel_enabled(t, ch))
      continue;
    for(uint8_t f = 1; f <= VTPS92682_FAULT_ISO; f <<= 1) {
      if(!(t->faults[ch] & f) || !fault_enabled(t, ch, f))
        continue;
      if(!fault_disables(f))
        continue;
      if(fault_latching(t, ch, f))
        t->latched[ch] = 1;
      else
        hold_off[ch] = 1;
    }
  }

  uint8_t a1, a2;
  active_bits(t, &a1, &a2);
  t->flt1 |= a1;
  t->flt2 |= a2;

  const int intpwm = t->regs[REG_CFG1] & CFG1_INTPWM;

  for(unsigned int ch = 0; ch < 2; ch++) {
    vtps92682_output_t o = {
      .on = channel_enabled(t, ch) && !t->latched[ch] && !hold_off[ch],
      .iadj = t->regs[REG_CH1IADJ + ch],
      .pwm = intpwm ? (t->regs[REG_CH1PWML + ch * 2] |
                       (t->regs[REG_CH1PWML + ch * 2 + 1] & 3) << 8)
                    : 1023,
    };
    if(memcmp(&o, &t->outputs[ch], sizeof(o))) {
      t->outputs[ch] = o;
      if(t->listener)
        t->listener(t->listener_opaque, t, ch);
    }
  }
}


static uint8_t
reg_read(const vtps92682_t *t, uint8_t addr)
{
  switch(addr) {
  case REG_FLT1:
    return t->flt1 | (t->pc ? FLT1_PC : 0);
  case REG_FLT2:
    return t->flt2;
  case REG_RESET:
    return 0;
  default:
    return addr <= REG_LAST ? t->regs[addr] : 0;
  }
}


static void
reset_registers(vtps92682_t *t)
{
  memcpy(t->regs, reg_defaults, sizeof(t->regs));
  t->latched[0] = t->latched[1] = 0;
}


static void
reg_write(vtps92682_t *t, uint8_t addr, uint8_t value)
{
  switch(addr) {
  case REG_FLT1:
  case REG_FLT2:
    return;  // Read only

  case REG_RESET:
    if(value == 0xc3)
      reset_registers(t);
    return;

  case REG_EN:
    value &= ~EN_FPINRST;  // Write-only, reads 0
    if(t->pc)
      value &= ~0x03;      // CHxEN blocked while PC is set
    for(unsigned int ch = 0; ch < 2; ch++) {
      if(value & (1 << ch))
        t->latched[ch] = 0;  // Re-programming the enable re-arms
    }
    t->regs[REG_EN] = value;
    return;

  default:
    if(addr <= REG_LAST)
      t->regs[addr] = value;
    // Writes to unlisted addresses are "not permitted", ignore them
    return;
  }
}


// End of the read response frame of an error-free FLT1/FLT2 read: clear
// the bits whose fault is gone
static void
clear_read_bits(vtps92682_t *t, uint8_t reg)
{
  uint8_t a1, a2;
  active_bits(t, &a1, &a2);
  if(reg == REG_FLT1) {
    t->flt1 &= a1;
    t->pc = 0;
  } else {
    t->flt2 &= a2;
  }
}


static void
decode_frame(vtps92682_t *t)
{
  const int misaligned = t->nbytes == 0 || (t->nbytes & 1);
  const uint16_t w = t->last16;
  const int write = w >> 15;
  const uint8_t addr = (w >> 9) & 0x3f;
  const uint8_t data = w & 0xff;
  const int parity_ok = __builtin_popcount(w) & 1;

  const int spe = misaligned || !parity_ok || (!write && data);
  if(spe)
    t->spi_errors++;

  if(write) {
    if(spe) {
      t->next_out = FRAME_POR;
      return;
    }
    reg_write(t, addr, data);
    t->next_out = (1 << 14) | (addr << 8) | data;
    return;
  }

  const uint8_t flt1 = reg_read(t, REG_FLT1);
  t->next_out = (spe << 15) | (0xc << 11) |
    (!!(flt1 & FLT1_RTO) << 10) | (!!(flt1 & FLT1_PC) << 9) |
    (!!(flt1 & FLT1_TW) << 8) | reg_read(t, addr);

  if(!spe && (addr == REG_FLT1 || addr == REG_FLT2))
    t->clear_after = addr;
}


static void
tps_select(vspi_dev_t *d, int mode)
{
  vtps92682_t *t = (vtps92682_t *)d;
  t->out = t->next_out;
  t->nbytes = 0;
  t->last16 = 0;
  t->mode = mode;
}


static uint8_t
tps_xfer(vspi_dev_t *d, uint8_t mosi)
{
  vtps92682_t *t = (vtps92682_t *)d;
  if(t->dead)
    return 0xff;

  uint8_t miso;
  if(t->nbytes < 2)
    miso = t->out >> (8 * (1 - t->nbytes));
  else
    miso = t->last16 >> 8;  // Received bits shift through

  t->last16 = (t->last16 << 8) | mosi;
  t->nbytes++;
  return miso;
}


static void
tps_deselect(vspi_dev_t *d)
{
  vtps92682_t *t = (vtps92682_t *)d;
  if(t->dead)
    return;

  // MOSI sampled on the rising edge, MISO changed on the falling edge
  // (Figure 7-22): mode 0. Another mode garbles the frame.
  if(t->mode != 0)
    t->nbytes = 0;

  if(t->clear_after) {
    clear_read_bits(t, t->clear_after);
    t->clear_after = 0;
  }

  decode_frame(t);
  update(t);
}


static const vspi_dev_ops_t vtps92682_ops = {
  .select = tps_select,
  .xfer = tps_xfer,
  .deselect = tps_deselect,
};


static void
power_on(vtps92682_t *t)
{
  reset_registers(t);
  t->pc = 1;
  t->flt1 = 0;
  t->flt2 = 0;
  t->next_out = FRAME_POR;
  t->clear_after = 0;
  update(t);
}


struct vtps92682 *
vtps92682_create(spi_t *bus, gpio_t cs)
{
  vtps92682_t *t = calloc(1, sizeof(vtps92682_t));
  t->dev.ops = &vtps92682_ops;
  t->bus = bus;
  power_on(t);
  if(vspi_attach(bus, cs, &t->dev))
    panic("vtps92682: chip select %d taken", cs);
  return t;
}


void
vtps92682_power_cycle(struct vtps92682 *t)
{
  t->bus->lock(t->bus, 1);
  power_on(t);
  t->bus->lock(t->bus, 0);
}


void
vtps92682_set_fault(struct vtps92682 *t, unsigned int ch, uint8_t faults,
                    int on)
{
  if(ch > 1)
    return;
  t->bus->lock(t->bus, 1);

  const uint8_t shared = faults & (VTPS92682_FAULT_TW | VTPS92682_FAULT_RTO);
  faults &= ~shared;

  if(on) {
    t->faults[ch] |= faults;
    t->shared |= shared;
  } else {
    t->faults[ch] &= ~faults;
    t->shared &= ~shared;
  }
  update(t);
  t->bus->lock(t->bus, 0);
}


void
vtps92682_set_dead(struct vtps92682 *t, int dead)
{
  t->bus->lock(t->bus, 1);
  t->dead = !!dead;
  t->bus->lock(t->bus, 0);
}


vtps92682_output_t
vtps92682_output(struct vtps92682 *t, unsigned int ch)
{
  vtps92682_output_t o = {};
  if(ch > 1)
    return o;
  t->bus->lock(t->bus, 1);
  o = t->outputs[ch];
  t->bus->lock(t->bus, 0);
  return o;
}


uint8_t
vtps92682_peek(struct vtps92682 *t, uint8_t reg)
{
  t->bus->lock(t->bus, 1);
  uint8_t r = reg_read(t, reg);
  t->bus->lock(t->bus, 0);
  return r;
}


unsigned int
vtps92682_spi_errors(struct vtps92682 *t)
{
  return t->spi_errors;
}


void
vtps92682_set_listener(struct vtps92682 *t,
                       void (*cb)(void *opaque, struct vtps92682 *t,
                                  unsigned int ch),
                       void *opaque)
{
  t->bus->lock(t->bus, 1);
  t->listener = cb;
  t->listener_opaque = opaque;
  t->bus->lock(t->bus, 0);
}
