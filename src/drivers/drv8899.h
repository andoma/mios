#pragma once

#include <mios/io.h>
#include <mios/stream.h>

// Register addresses
#define DRV8899_FAULT_STATUS  0x00
#define DRV8899_DIAG_STATUS1  0x01
#define DRV8899_DIAG_STATUS2  0x02
#define DRV8899_CTRL1         0x03
#define DRV8899_CTRL2         0x04
#define DRV8899_CTRL3         0x05
#define DRV8899_CTRL4         0x06
#define DRV8899_CTRL5         0x07
#define DRV8899_CTRL6         0x08
#define DRV8899_CTRL7         0x09
#define DRV8899_CTRL8         0x0A

// FAULT_STATUS bits (0x00)
#define DRV8899_FAULT_FAULT     0x80
#define DRV8899_FAULT_SPI_ERROR 0x40
#define DRV8899_FAULT_UVLO      0x20
#define DRV8899_FAULT_CPUV      0x10
#define DRV8899_FAULT_OCP       0x08
#define DRV8899_FAULT_TF        0x02
#define DRV8899_FAULT_OL        0x01

// DIAG_STATUS2 bits (0x02)
#define DRV8899_DIAG2_UTW       0x80
#define DRV8899_DIAG2_OTW       0x40
#define DRV8899_DIAG2_OTS       0x20
#define DRV8899_DIAG2_OL_B      0x02
#define DRV8899_DIAG2_OL_A      0x01

// CTRL1 fields (0x03)
#define DRV8899_CTRL1_TRQ_DAC(x)   (((x) & 0xf) << 4)
#define DRV8899_CTRL1_SLEW_RATE(x)  ((x) & 0x3)

// CTRL2 fields (0x04)
#define DRV8899_CTRL2_DIS_OUT       0x80
#define DRV8899_CTRL2_TOFF(x)      (((x) & 0x3) << 3)
#define DRV8899_CTRL2_DECAY(x)      ((x) & 0x7)

// CTRL2 DECAY modes
#define DRV8899_DECAY_SLOW_SLOW     0
#define DRV8899_DECAY_SLOW_MIX30    1
#define DRV8899_DECAY_SLOW_MIX60    2
#define DRV8899_DECAY_SLOW_FAST     3
#define DRV8899_DECAY_MIX30_MIX30   4
#define DRV8899_DECAY_MIX60_MIX60   5
#define DRV8899_DECAY_SMART_DYN     6
#define DRV8899_DECAY_SMART_RIPPLE  7

// CTRL3 fields (0x05)
#define DRV8899_CTRL3_DIR           0x80
#define DRV8899_CTRL3_STEP          0x40
#define DRV8899_CTRL3_SPI_DIR       0x20
#define DRV8899_CTRL3_SPI_STEP      0x10
#define DRV8899_CTRL3_MICROSTEP(x)  ((x) & 0xf)

// CTRL3 MICROSTEP_MODE values
#define DRV8899_STEP_FULL_100       0x0
#define DRV8899_STEP_FULL_71        0x1
#define DRV8899_STEP_NC_HALF        0x2
#define DRV8899_STEP_HALF           0x3
#define DRV8899_STEP_1_4            0x4
#define DRV8899_STEP_1_8            0x5
#define DRV8899_STEP_1_16           0x6
#define DRV8899_STEP_1_32           0x7
#define DRV8899_STEP_1_64           0x8
#define DRV8899_STEP_1_128          0x9
#define DRV8899_STEP_1_256          0xa

// CTRL4 fields (0x06)
#define DRV8899_CTRL4_CLR_FLT       0x80
#define DRV8899_CTRL4_LOCK(x)      (((x) & 0x7) << 4)
#define DRV8899_CTRL4_EN_OL         0x08
#define DRV8899_CTRL4_OCP_MODE      0x04
#define DRV8899_CTRL4_OTSD_MODE     0x02
#define DRV8899_CTRL4_TW_REP        0x01

#define DRV8899_LOCK_UNLOCK         0x3
#define DRV8899_LOCK_LOCK           0x6

// SPI status byte bits (upper 8 bits of SDO response)
#define DRV8899_STATUS_UVLO         0x20
#define DRV8899_STATUS_CPUV         0x10
#define DRV8899_STATUS_OCP          0x08
#define DRV8899_STATUS_TF           0x02
#define DRV8899_STATUS_OL           0x01

struct drv8899;

struct drv8899 *drv8899_create(spi_t *bus, gpio_t cs);

error_t drv8899_write_reg(struct drv8899 *d, uint8_t reg, uint8_t value);

int drv8899_read_reg(struct drv8899 *d, uint8_t reg);

// Initialize for normal operation: clear faults, set decay mode,
// microstepping and torque DAC. Outputs are enabled after this call.
error_t drv8899_init(struct drv8899 *d, uint8_t microstep_mode,
                     uint8_t decay_mode, uint8_t trq_dac);

error_t drv8899_set_microstep(struct drv8899 *d, uint8_t mode);

error_t drv8899_set_trq_dac(struct drv8899 *d, uint8_t trq_dac);

error_t drv8899_clear_faults(struct drv8899 *d);

int drv8899_get_faults(struct drv8899 *d);

void drv8899_print_status(struct drv8899 *d, stream_t *st);
