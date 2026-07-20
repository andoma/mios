#pragma once

#include "jlink.h"

// ARM ADIv5 debug port / memory access port over SWD

typedef struct dap dap_t;

dap_t *dap_create(jlink_t *jl);

void dap_destroy(dap_t *d);

const char *dap_errmsg(dap_t *d);

// Line reset + JTAG-to-SWD switch + debug domain power-up
int dap_connect(dap_t *d, uint32_t *dpidr);

int dap_dp_read(dap_t *d, int reg, uint32_t *value);

int dap_dp_write(dap_t *d, int reg, uint32_t value);

int dap_ap_read(dap_t *d, int apsel, int reg, uint32_t *value);

int dap_ap_write(dap_t *d, int apsel, int reg, uint32_t value);

// Memory access via MEM-AP (apsel 0). Returns 0, or -1 with errmsg set.
// dap_mem_init() must be called once before the other mem functions.
// Returns DeviceEn status via *device_en (0 = AP locked out).
int dap_mem_init(dap_t *d, int *device_en);

int dap_mem_read32(dap_t *d, uint32_t addr, uint32_t *value);

int dap_mem_write32(dap_t *d, uint32_t addr, uint32_t value);

int dap_mem_write_block(dap_t *d, uint32_t addr, const void *data, size_t len);

int dap_mem_read_block(dap_t *d, uint32_t addr, void *data, size_t len);
