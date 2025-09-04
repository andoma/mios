#pragma once

#include <mios/error.h>
#include <stdint.h>

struct stream;
struct block_iface;

/*
 * Prepares a SPI flash for OTA by partitioning it into two partitions
 *
 * One will be used for OTA upgrades and the second will be a regular
 * little-fs filesystem
 *
 * Implemented by each platform
 *
 */
void ota_partition_spiflash(struct block_iface *flash);

/*
 * Returns a stream we can write an ELF file to (for example from http_get())
 *
 * Implemented by each platform, as it need to call elf_to_bin() and
 * bin_to_ota() with the correct parameters which depends on
 * memory/address-map-layout etc.
 *
 */
struct stream *ota_get_stream(void);


/*
 * Optionally implemented by application to prohibit upgrades
 * Checked just before starting. Return a reasonable error code
 * to prohibit upgrade from starting (Can be used to avoid rebooting
 * if BOOT0 is held high on STM32, etc)
 */
error_t ota_prohibit_upgrade(void);


/*
 * Write out a binary stream to spiflash and generate upgrade headers, etc
 * Used by ota_get_stream()
 */
struct stream *bin_to_ota(struct block_iface *output, uint32_t block_offset);

