#pragma once
#include <mios/io.h>
/* Virtual I2C bus: a 256-byte register file per 7-bit address. The mios
   side sees a normal i2c_t; the harness pokes registers through the ABI in
   libmios.c to model whatever sensors/actuators the app expects. */
typedef struct vi2c vi2c_t;
i2c_t *vi2c_bus(void);                       /* the mios-facing bus */
void vi2c_set_reg(uint8_t addr, uint8_t reg, uint8_t val);
uint8_t vi2c_get_reg(uint8_t addr, uint8_t reg);
