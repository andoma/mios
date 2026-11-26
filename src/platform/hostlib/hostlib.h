#pragma once
/*
 * What the hostlib platform offers an application's board file: the
 * virtual buses a harness drives through the ABI in libmios.h. A hostlib
 * board file creates its drivers on these exactly like a real board file
 * does on stm32h7_spi_create() and friends.
 *
 * The buses are the host platform's (platform/host/vi2c.c, vspi.c) with
 * a register file (vregfile.c) on each address and chip select the
 * harness reaches; a board file may create more buses and attach models
 * of its own chips itself.
 */
#include <mios/io.h>

#include "vi2c.h"
#include "vspi.h"

/* Chip selects of the SPI register files: the GPIOs 0 .. HOSTLIB_SPI_NDEV-1,
   made push-pull outputs at 1 when the bus is created. Pass one as the nss
   of a driver on hostlib_spi(). */
#define HOSTLIB_SPI_NDEV 4

/* The I2C bus: a register file at every 7-bit address (so every address
   ACKs), the harness's mios_sim_i2c_*(). Created on first call. */
i2c_t *hostlib_i2c(void);

/* The SPI bus: a register file on each chip select 0..HOSTLIB_SPI_NDEV-1,
   the harness's mios_sim_spi_*(). Created on first call. */
spi_t *hostlib_spi(void);

struct vcan;

/* The virtual CAN interface (a real can_netif, so DSIG/VLLP run
   unmodified). Created and brought up on first call; call it from main()
   so the harness has something to talk to. MTU 64 (FDCAN). */
struct vcan *hostlib_can(void);
