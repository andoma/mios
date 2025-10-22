#pragma once

#include <mios/error.h>
#include <stdint.h>
#include <stddef.h>

error_t bpmp_xfer(uint32_t mrq,
                  void *in, size_t in_size,
                  void *out, size_t *out_size);

error_t bpmp_powergate_set(int id, int on);

error_t bpmp_rst_set(int id, int on);

error_t bpmp_rst_toggle(int id);

error_t bpmp_pcie_set(int id, int on);

error_t bpmp_get_temperature(int id, int *millideg);
