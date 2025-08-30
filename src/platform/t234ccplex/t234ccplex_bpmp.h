#pragma once

#include <mios/error.h>
#include <stdint.h>
#include <stddef.h>

error_t bpmp_xfer(uint32_t mrq,
                  void *in, size_t in_size,
                  void *out, size_t *out_size);
