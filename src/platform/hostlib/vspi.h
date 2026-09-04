#pragma once
#include <mios/io.h>
/* Virtual SPI bus: a 256-byte register file per chip select. The mios side
   sees a normal spi_t; the harness pokes registers through the ABI in
   libmios.c to model whatever sensors the app expects.

   Wire protocol is the common register-file convention (ICM-42688, MPU9250,
   BMI, LSM... and most other sensor chips): the first byte is the register
   address, bit 7 set for a read. A read then clocks out consecutive
   registers; a write stores the following bytes to consecutive registers.
   The chip select (the nss gpio the driver was created with) selects the
   register file, so the app's hostlib board file picks nss 0..VSPI_NDEV-1.

   Not for command/response parts with their own protocol (SPI NOR flash
   etc.): those need a dedicated model, see platform/host/vspiflash.c. */
#define VSPI_NDEV 4
spi_t *vspi_bus(void);                        /* the mios-facing bus */
void vspi_set_reg(uint8_t dev, uint8_t reg, uint8_t val);
uint8_t vspi_get_reg(uint8_t dev, uint8_t reg);
