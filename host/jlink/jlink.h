#pragma once

#include <stdint.h>
#include <stddef.h>

// SEGGER J-Link USB probe. Speaks the vendor bulk protocol directly,
// no J-Link software required.

typedef struct jlink jlink_t;

// Open first probe (or match USB serial number string if serial != NULL)
jlink_t *jlink_open(const char *serial);

void jlink_close(jlink_t *jl);

const char *jlink_errmsg(jlink_t *jl);

const char *jlink_version(jlink_t *jl);

const char *jlink_serial(jlink_t *jl);

int jlink_select_swd(jlink_t *jl);

// Target reference voltage in millivolts (0 = target unpowered)
int jlink_target_voltage(jlink_t *jl);

int jlink_set_speed_khz(jlink_t *jl, unsigned int khz);

// Max number of bits per jlink_swd_io() call
size_t jlink_swd_max_bits(const jlink_t *jl);

// Clock nbits on SWCLK. dir/out/in are LSB-first packed bit arrays.
// dir bit 1 = host drives SWDIO with corresponding out bit,
// dir bit 0 = SWDIO sampled from target into corresponding in bit.
// in may be NULL if no bits are read.
int jlink_swd_io(jlink_t *jl, const uint8_t *dir, const uint8_t *out,
                 uint8_t *in, size_t nbits);
