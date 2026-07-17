#pragma once

#include <stddef.h>
#include <stdint.h>

// CRACEN true random number generator. read blocks (busy-polls) until the
// requested number of bytes has been produced.
void nrf54l_trng_init(void);
void nrf54l_trng_read(uint8_t *buf, size_t len);
