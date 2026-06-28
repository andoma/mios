#pragma once

#include "mcp_server.h"
#include <stddef.h>
#include <stdint.h>

// Transport abstraction for the MCP message protocol (type byte + payload).
// Two backends:
//   - USB:   one bulk transfer per message (default)
//   - Serial: one HDLC frame (CRC32) per message, used when ctx->serial is set
//
// Each tool call opens a transport, exchanges messages, and closes it.

typedef struct mcp_xport mcp_xport_t;

// Open a transport. subclass selects the USB MCP interface (ignored for
// serial). On failure returns NULL and sets *errstr.
mcp_xport_t *mcp_xport_open(mcp_context_t *ctx, uint8_t subclass,
                            const char **errstr);

// Largest payload (excluding the type byte) the transport accepts/returns.
size_t mcp_xport_max_payload(mcp_xport_t *x);

// Send one MCP message (type byte + payload). Returns 0 on success.
int mcp_xport_send(mcp_xport_t *x, const uint8_t *msg, size_t len);

// Receive one MCP message into buf. Returns message length (>=1), 0 on
// timeout, or -1 on error.
int mcp_xport_recv(mcp_xport_t *x, uint8_t *buf, size_t cap, int timeout_ms);

void mcp_xport_close(mcp_xport_t *x);
