#pragma once

#include <stdint.h>

int base64_encode(char *out, int out_size, const uint8_t *in, int in_size);
