#pragma once
#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
/*
 * Public ABI of a mios shared object (the "hostlib" platform). Everything
 * else in the .so is hidden. A host harness loads this with dlmopen() and
 * drives mios in lockstep virtual time, exchanging sensor/actuator state
 * through the virtual buses: register files on I2C and SPI, DSIG frames on
 * CAN. All calls are made between steps, from the harness's thread; none
 * of them block or call into the mios kernel.
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

/* Virtual SPI register file access. dev is the chip select (the nss the
   driver was created with), 0..VSPI_NDEV-1. See vspi.h for the wire
   protocol the firmware side sees. */
void    mios_sim_spi_set(uint8_t dev, uint8_t reg, uint8_t val);
uint8_t mios_sim_spi_get(uint8_t dev, uint8_t reg);

/* Virtual CAN, DSIG frames: (signal id, payload). recv returns the next
   frame the firmware transmitted, or -1 if none is queued. send queues a
   frame for the firmware; it is delivered on the next step. Both return
   -1 if the app never created the interface (hostlib_can()). */
ssize_t mios_sim_can_recv(uint32_t *id, void *buf, size_t buflen);
int     mios_sim_can_send(uint32_t id, const void *payload, size_t len);

#ifdef __cplusplus
}
#endif
