#pragma once

#include "dap.h"

int nrf54l_identify(dap_t *d, char *buf, size_t buflen);

int nrf54l_reset_halt(dap_t *d);

int nrf54l_program(dap_t *d, uint32_t addr, const void *data, size_t len);

int nrf54l_reset_run(dap_t *d);

// CTRL-AP ERASEALL: wipes RRAM + UICR and disables approtect
int nrf54l_recover(dap_t *d);
