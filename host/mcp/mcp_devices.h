/*
 * Named device list for MCP-over-VLLP, loaded from a JSON file (see
 * mcp_devices_load()) so mios-mcp itself never has to know any
 * project-specific board names -- those live entirely in the file a
 * given project points MIOS_MCP_DEVICES at.
 *
 * File format: a JSON array of
 *   { "name": "...", "transport": "...", "vllp_tx": ..., "vllp_rx": ... }
 * transport is a dsig transport address (see dsig_transport.h), e.g.
 * "file:///tmp/fcmon.sock". vllp_tx/vllp_rx may be a JSON number or a
 * "0x..." string.
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  char *name;
  char *transport;
  uint32_t vllp_tx;
  uint32_t vllp_rx;
} mcp_device_t;

/* Loads devices from path. On success returns the count and sets *out
 * (caller frees with mcp_devices_free()). On failure returns -1 and
 * sets *errstr to a static message; *out is untouched.
 */
int mcp_devices_load(const char *path, mcp_device_t **out,
                     const char **errstr);

void mcp_devices_free(mcp_device_t *devices, int count);

/* NULL if no device with that name exists. */
const mcp_device_t *mcp_devices_find(const mcp_device_t *devices, int count,
                                     const char *name);

#ifdef __cplusplus
}
#endif
