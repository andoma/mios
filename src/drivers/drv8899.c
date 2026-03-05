#include "drv8899.h"

#include <stdlib.h>
#include <malloc.h>
#include <string.h>

#include <stdio.h>

#include <mios/eventlog.h>

// https://www.ti.com/lit/ds/symlink/drv8899-q1.pdf

typedef struct drv8899 {
  spi_t *spi;
  uint32_t spi_config;
  gpio_t cs;
  uint8_t tx[2];
  uint8_t rx[2];
} drv8899_t;


// SPI frame format (16-bit):
//   SDI: [0][W0][A4:A0][X][D7:D0]
//     W0=0 for write, W0=1 for read
//   SDO: [1][1][UVLO][CPUV][OCP][RSVD][TF][OL] [Report D7:D0]

error_t
drv8899_write_reg(drv8899_t *d, uint8_t reg, uint8_t value)
{
  d->tx[0] = (reg & 0x1f) << 1;
  d->tx[1] = value;
  return d->spi->rw(d->spi, d->tx, NULL, 2, d->cs, d->spi_config);
}


int
drv8899_read_reg(drv8899_t *d, uint8_t reg)
{
  d->tx[0] = 0x40 | ((reg & 0x1f) << 1);
  d->tx[1] = 0;
  error_t err = d->spi->rw(d->spi, d->tx, d->rx, 2, d->cs, d->spi_config);
  if(err)
    return err;
  return d->rx[1];
}


struct drv8899 *
drv8899_create(spi_t *bus, gpio_t cs)
{
  drv8899_t *d = xalloc(sizeof(drv8899_t), 0, MEM_TYPE_DMA | MEM_CLEAR);

  d->spi = bus;
  d->cs = cs;
  d->spi_config = bus->get_config(bus, SPI_CPHA, 1000000);

  int rev = drv8899_read_reg(d, DRV8899_CTRL8);
  if(rev < 0) {
    evlog(LOG_ERR, "drv8899: SPI read failed");
    free(d);
    return NULL;
  }

  evlog(LOG_DEBUG, "drv8899: rev_id=0x%x", rev & 0xf);
  return d;
}


error_t
drv8899_init(drv8899_t *d, uint8_t microstep_mode,
             uint8_t decay_mode, uint8_t trq_dac)
{
  error_t err;

  // Clear any latched faults
  err = drv8899_write_reg(d, DRV8899_CTRL4,
                          DRV8899_CTRL4_CLR_FLT |
                          DRV8899_CTRL4_LOCK(DRV8899_LOCK_UNLOCK));
  if(err)
    return err;

  // CTRL1: Torque DAC + default slew rate (10 V/us)
  err = drv8899_write_reg(d, DRV8899_CTRL1,
                          DRV8899_CTRL1_TRQ_DAC(trq_dac));
  if(err)
    return err;

  // CTRL2: Enable outputs, set decay mode, default TOFF (16 us)
  err = drv8899_write_reg(d, DRV8899_CTRL2,
                          DRV8899_CTRL2_TOFF(1) |
                          DRV8899_CTRL2_DECAY(decay_mode));
  if(err)
    return err;

  // CTRL3: Use pin-based STEP/DIR, set microstepping mode
  err = drv8899_write_reg(d, DRV8899_CTRL3,
                          DRV8899_CTRL3_MICROSTEP(microstep_mode));
  if(err)
    return err;

  // CTRL4: Unlock registers, enable open-load detection,
  //        auto-retry on OCP, auto-recovery on overtemp
  err = drv8899_write_reg(d, DRV8899_CTRL4,
                          DRV8899_CTRL4_LOCK(DRV8899_LOCK_UNLOCK) |
                          DRV8899_CTRL4_EN_OL |
                          DRV8899_CTRL4_OCP_MODE |
                          DRV8899_CTRL4_OTSD_MODE |
                          DRV8899_CTRL4_TW_REP);
  if(err)
    return err;

  return 0;
}


error_t
drv8899_set_microstep(drv8899_t *d, uint8_t mode)
{
  if(mode > DRV8899_STEP_1_256)
    return ERR_INVALID_ARGS;

  return drv8899_write_reg(d, DRV8899_CTRL3,
                           DRV8899_CTRL3_MICROSTEP(mode));
}


error_t
drv8899_set_trq_dac(drv8899_t *d, uint8_t trq_dac)
{
  if(trq_dac > 0xf)
    return ERR_INVALID_ARGS;

  return drv8899_write_reg(d, DRV8899_CTRL1,
                           DRV8899_CTRL1_TRQ_DAC(trq_dac));
}


error_t
drv8899_clear_faults(drv8899_t *d)
{
  return drv8899_write_reg(d, DRV8899_CTRL4,
                           DRV8899_CTRL4_CLR_FLT |
                           DRV8899_CTRL4_LOCK(DRV8899_LOCK_UNLOCK));
}


int
drv8899_get_faults(drv8899_t *d)
{
  return drv8899_read_reg(d, DRV8899_FAULT_STATUS);
}


static const char *const microstep_names[] = {
  [0x0] = "Full step 100%",
  [0x1] = "Full step 71%",
  [0x2] = "Non-circular 1/2",
  [0x3] = "1/2 step",
  [0x4] = "1/4 step",
  [0x5] = "1/8 step",
  [0x6] = "1/16 step",
  [0x7] = "1/32 step",
  [0x8] = "1/64 step",
  [0x9] = "1/128 step",
  [0xa] = "1/256 step",
};

static const char *const decay_names[] = {
  [0] = "Slow/Slow",
  [1] = "Slow/Mixed 30%",
  [2] = "Slow/Mixed 60%",
  [3] = "Slow/Fast",
  [4] = "Mixed 30%/Mixed 30%",
  [5] = "Mixed 60%/Mixed 60%",
  [6] = "Smart tune Dynamic",
  [7] = "Smart tune Ripple",
};

static const int toff_us[] = { 7, 16, 24, 32 };

static const int slew_rate_vus[] = { 10, 35, 50, 105 };

static const char *const trq_dac_pct[] = {
  "100", "93.75", "87.5", "81.25", "75", "68.75", "62.5", "56.25",
  "50", "43.75", "37.5", "31.25", "25", "18.75", "12.5", "6.25",
};


void
drv8899_print_status(drv8899_t *d, stream_t *st)
{
  int val;

  val = drv8899_read_reg(d, DRV8899_FAULT_STATUS);
  if(val < 0) {
    stprintf(st, "drv8899: SPI read error\n");
    return;
  }
  stprintf(st, "Fault status: 0x%02x", val);
  if(val & DRV8899_FAULT_FAULT)     stprintf(st, " FAULT");
  if(val & DRV8899_FAULT_SPI_ERROR) stprintf(st, " SPI_ERR");
  if(val & DRV8899_FAULT_UVLO)      stprintf(st, " UVLO");
  if(val & DRV8899_FAULT_CPUV)      stprintf(st, " CPUV");
  if(val & DRV8899_FAULT_OCP)       stprintf(st, " OCP");
  if(val & DRV8899_FAULT_TF)        stprintf(st, " TF");
  if(val & DRV8899_FAULT_OL)        stprintf(st, " OL");
  stprintf(st, "\n");

  val = drv8899_read_reg(d, DRV8899_DIAG_STATUS1);
  if(val >= 0) {
    stprintf(st, "Diag 1:       0x%02x", val);
    if(val & 0x80) stprintf(st, " OCP_LS2_B");
    if(val & 0x40) stprintf(st, " OCP_HS2_B");
    if(val & 0x20) stprintf(st, " OCP_LS1_B");
    if(val & 0x10) stprintf(st, " OCP_HS1_B");
    if(val & 0x08) stprintf(st, " OCP_LS2_A");
    if(val & 0x04) stprintf(st, " OCP_HS2_A");
    if(val & 0x02) stprintf(st, " OCP_LS1_A");
    if(val & 0x01) stprintf(st, " OCP_HS1_A");
    stprintf(st, "\n");
  }

  val = drv8899_read_reg(d, DRV8899_DIAG_STATUS2);
  if(val >= 0) {
    stprintf(st, "Diag 2:       0x%02x", val);
    if(val & DRV8899_DIAG2_UTW) stprintf(st, " UTW");
    if(val & DRV8899_DIAG2_OTW) stprintf(st, " OTW");
    if(val & DRV8899_DIAG2_OTS) stprintf(st, " OTS");
    if(val & DRV8899_DIAG2_OL_B) stprintf(st, " OL_B");
    if(val & DRV8899_DIAG2_OL_A) stprintf(st, " OL_A");
    stprintf(st, "\n");
  }

  val = drv8899_read_reg(d, DRV8899_CTRL1);
  if(val >= 0) {
    stprintf(st, "CTRL1:        0x%02x  TRQ_DAC=%s%%  Slew=%d V/us\n",
             val, trq_dac_pct[(val >> 4) & 0xf], slew_rate_vus[val & 0x3]);
  }

  val = drv8899_read_reg(d, DRV8899_CTRL2);
  if(val >= 0) {
    stprintf(st, "CTRL2:        0x%02x  DIS_OUT=%d  TOFF=%d us  Decay=%s\n",
             val, (val >> 7) & 1, toff_us[(val >> 3) & 0x3],
             decay_names[val & 0x7]);
  }

  val = drv8899_read_reg(d, DRV8899_CTRL3);
  if(val >= 0) {
    int ms = val & 0xf;
    stprintf(st, "CTRL3:        0x%02x  DIR=%d  STEP=%d  SPI_DIR=%d  SPI_STEP=%d  Microstep=%s\n",
             val, (val >> 7) & 1, (val >> 6) & 1,
             (val >> 5) & 1, (val >> 4) & 1,
             ms <= 0xa ? microstep_names[ms] : "Reserved");
  }

  val = drv8899_read_reg(d, DRV8899_CTRL4);
  if(val >= 0) {
    stprintf(st, "CTRL4:        0x%02x  LOCK=%d  EN_OL=%d  OCP_MODE=%d  OTSD_MODE=%d  TW_REP=%d\n",
             val, (val >> 4) & 0x7, (val >> 3) & 1,
             (val >> 2) & 1, (val >> 1) & 1, val & 1);
  }

  val = drv8899_read_reg(d, DRV8899_CTRL8);
  if(val >= 0) {
    stprintf(st, "CTRL8:        0x%02x  REV_ID=%d\n", val, val & 0xf);
  }
}
