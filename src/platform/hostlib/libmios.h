#pragma once
#include <stdint.h>
/*
 * Public ABI of a mios shared object (the "hostlib" platform). Everything
 * else in the .so is hidden. A host harness loads this with dlmopen() and
 * drives mios in lockstep virtual time, exchanging sensor/actuator state
 * through the virtual buses (here: a virtual I2C register file).
 */
#ifdef __cplusplus
extern "C" {
#endif

/* Boot mios to its first idle. Call once after (dl)opening the object. */
void mios_sim_boot(void);

/* Run the firmware until it is idle again at or before now + dt_us. */
void mios_sim_step(uint64_t dt_us);

/* Current virtual time, microseconds. */
uint64_t mios_sim_time(void);

/* Virtual I2C register file access (the harness's window on simulated
   sensors and any values the firmware writes back). addr is the 7-bit
   I2C address. */
void    mios_sim_i2c_set(uint8_t addr, uint8_t reg, uint8_t val);
uint8_t mios_sim_i2c_get(uint8_t addr, uint8_t reg);

#ifdef __cplusplus
}
#endif
