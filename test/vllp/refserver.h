/*
 * Host-side reference VLLP server offering the same test services the
 * MCU has: "echo", "chargen", "discard". Anything else -> NOT_FOUND.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "vllp.h"

#define REFSERVER_FRAGMENT_SIZE 508  /* matches PBUF_DATA_SIZE - 4 on mios */

typedef struct refserver refserver_t;

refserver_t *refserver_create(int mtu, int timeout, uint32_t flags,
                              void *tx_opaque,
                              void (*tx)(void *opaque, const void *data,
                                         size_t len),
                              void *log_opaque,
                              void (*log)(void *opaque, int level,
                                          const char *msg));

vllp_t *refserver_vllp(refserver_t *rs);

/* Channels currently open (accepted and not yet closed) */
int refserver_open_channels(refserver_t *rs);

void refserver_destroy(refserver_t *rs);
