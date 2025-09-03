#pragma once

#include <stdint.h>

struct stream;
struct block_iface;

struct stream *bin_to_ota(struct block_iface *output, uint32_t block_offset);

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

