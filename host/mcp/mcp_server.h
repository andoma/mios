#pragma once

#include <stdint.h>
#include <libusb.h>
#include "cJSON.h"

typedef struct mcp_context {
  libusb_context *usb;
  uint16_t usb_vid;
  uint16_t usb_pid;  // 0 = match any
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
