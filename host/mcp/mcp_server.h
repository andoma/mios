#pragma once

#include <stdint.h>
#include <libusb.h>
#include "cJSON.h"
#include "mcp_devices.h"

typedef struct mcp_context {
  libusb_context *usb;
  uint16_t usb_vid;
  uint16_t usb_pid;  // 0 = match any
  char *serial;          // serial device path, or "auto" to detect
  char *serial_resolved; // cached path that "auto" resolved to

  // MCP-over-VLLP target, set via the "configure" tool (either directly,
  // or by naming an entry from `devices`). vllp_transport == NULL means
  // "not configured", i.e. use usb_vid/usb_pid or serial as before.
  char *vllp_transport;  // dsig transport address, e.g. "file:///tmp/fcmon.sock"
  uint32_t vllp_tx, vllp_rx;
  int vllp_mtu;          // 0 = default (64)
  int vllp_timeout_s;    // 0 = default (3)

  // Named devices loaded from $MIOS_MCP_DEVICES, if set. Project-specific;
  // mios itself never hardcodes anything from this list.
  mcp_device_t *devices;
  int num_devices;
} mcp_context_t;

// Tool handler function type.
// Returns a cJSON object that is the "content" array for the tool result.
// On error, return NULL and set *errstr to a static/allocated error message.
typedef cJSON *(*mcp_tool_handler_t)(mcp_context_t *ctx,
                                     const cJSON *params,
                                     const char **errstr);

typedef struct mcp_tool {
  const char *name;
  const char *description;
  const cJSON *input_schema;  // JSON Schema object (built at init)
  mcp_tool_handler_t handler;
} mcp_tool_t;

// Tool registration (called from each tool's init)
void mcp_register_tool(mcp_tool_t *tool);

// Helper: create a text content result (caller must free)
cJSON *mcp_text_result(const char *text);

// Helper: create a text content result with printf formatting
cJSON *mcp_text_resultf(const char *fmt, ...)
  __attribute__((format(printf, 1, 2)));
