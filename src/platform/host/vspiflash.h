#pragma once

/*
 * Virtual SPI NOR flash chip, for test suites.
 *
 * Models a plain SPI NOR part (SFDP geometry discovery, 0xAB release/id,
 * 0x05 status, 0x06 write-enable, 0x03 read, 0x02 page program, 0x20 4 kB
 * sector erase, 0xB9 deep-power-down) behind an spi_t. Bind the *real*
 * production driver to it with spiflash_create(bus, 0) and you get a
 * block_iface backed by an in-memory chip -- the same code path that runs
 * on the STM32 flashes, exercised on the host.
 *
 * All operations complete instantly (no real erase/program timing); under
 * virtual time the driver's busy-waits are free anyway.
 */

#include <stddef.h>

struct spi;

// size must be a multiple of 4096 and < 16 MB (3-byte addressing).
struct spi *vspiflash_create(size_t size);


// ---- Fault injection (test control) ----

// Fail the (ops+1)-th program/erase operation, and every one after it, with
// an I/O error (simulates a flash that errors mid-write). ops < 0 disables.
void vspiflash_fail_after(struct spi *bus, int ops);

// Flip a bit in the next byte programmed (simulates a bad cell: the write
// "succeeds" but reads back wrong). One-shot, self-clearing.
void vspiflash_corrupt_next_write(struct spi *bus);

// Clear a bit at addr in the backing store (simulates post-write bit-rot on
// a cell that already held data). Directly pokes the array.
void vspiflash_poke(struct spi *bus, uint32_t addr);
