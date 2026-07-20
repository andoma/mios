#pragma once

#include <stdio.h>
#include <stddef.h>

// Common firmware flashing API. Backends: J-Link/SWD (nRF54L),
// STM32 USB DFU, OpenOCD (ST-Link). Frontends: mios-flash CLI
// (used by 'make flash') and the mios-mcp MCP server.

// Growable log buffer. If tee is set, lines are also printed there as
// they happen (CLI); the MCP frontend returns the buffer as tool output.
typedef struct flash_log {
  char *buf;
  size_t len;
  size_t cap;
  FILE *tee;
} flash_log_t;

void flash_log_init(flash_log_t *l, FILE *tee);

void flash_log_free(flash_log_t *l);

void flash_logf(flash_log_t *l, const char *fmt, ...)
  __attribute__((format(printf, 2, 3)));

#define FLASH_NO_VERIFY  0x1  // Skip readback verification (jlink)
#define FLASH_NO_RUN     0x2  // Leave target halted after programming
#define FLASH_RECOVER    0x4  // Erase-all/unlock before programming (jlink)
#define FLASH_FORCE      0x8  // Flash even if build ID matches (dfu)
#define FLASH_RESET_ONLY 0x10 // Don't program, just reset the target

typedef struct flash_params {
  const char *elf_path;     // Required unless FLASH_RESET_ONLY
  const char *method;       // "jlink", "dfu", "openocd" or NULL/"auto"
  const char *cmdline;      // Boot cmdline deposited in RAM (dfu)
  const char *serial;       // Probe USB serial number (jlink)
  const char *openocd_host; // NULL = 127.0.0.1
  int openocd_port;         // 0 = 6666
  unsigned int swd_khz;     // 0 = 4000
  unsigned int flags;
} flash_params_t;

// Returns 0 on success, -1 on failure. Details are in the log either way.
int flash_run(const flash_params_t *p, flash_log_t *log);

// Backends (exposed for direct use; flash_run dispatches to these)
int flash_jlink(const flash_params_t *p, flash_log_t *log);

int flash_dfu(const flash_params_t *p, flash_log_t *log);

int flash_openocd(const flash_params_t *p, flash_log_t *log);
