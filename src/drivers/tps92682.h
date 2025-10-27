#pragma once

#include <mios/io.h>


#define TPS92682_EN   0x00
#define TPS92682_CFG1 0x01
#define TPS92682_CFG2 0x02
#define TPS92682_FM   0x05

#define TPS92682_CH1ADJ 0x07
#define TPS92682_CH2ADJ 0x08

#define TPS92682_CHxADJ(x) (0x07 + (x))

#define TPS92682_CH1PWML 0x0a
#define TPS92682_CH1PWMH 0x0b
#define TPS92682_CH2PWML 0x0c
#define TPS92682_CH2PWMH 0x0d

#define TPS92682_CHxPWML(x) (0x0a + (x) * 2)
#define TPS92682_CHxPWMH(x) (0x0b + (x) * 2)

#define TPS92682_FLT1  0x11
#define TPS92682_FLT2  0x12
#define TPS92682_FEN1  0x13
#define TPS92682_FEN2  0x14

#define TPS92682_FLATEN 0x15

#define TPS92682_RESET 0x26


#define TPS92682_FLT1_RTO   0x80  // RT pin open
#define TPS92682_FLT1_PC    0x20  // Power-cycled
#define TPS92682_FLT1_TW    0x10  // Thermal warning
#define TPS92682_FLT1_CH2OV 0x08
#define TPS92682_FLT1_CH1OV 0x04
#define TPS92682_FLT1_CH2UV 0x02
#define TPS92682_FLT1_CH1UV 0x01


struct tps92682;

struct tps92682 *tps92682_create(spi_t *spi, gpio_t cs);

error_t tps92682_init(struct tps92682 *drv, int clear_faults);

error_t tps92682_set_ilimit_raw(struct tps92682 *drv, unsigned int channel,
                                unsigned int value);

error_t tps92682_write_reg(struct tps92682 *drv, uint8_t reg, uint8_t value);

int tps92682_read_reg(struct tps92682 *drv, uint8_t reg);

error_t tps92682_set_spread_specturm(struct tps92682 *t,
                                     unsigned int magnitude);
