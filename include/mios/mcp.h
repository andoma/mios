#pragma once

struct stream;

// Run a Mios Control Protocol (MCP) endpoint over an HDLC-framed byte
// stream, typically a UART. Each request and response is one HDLC frame
// (CRC32 protected). The message format matches the USB MCP interface:
// a one-byte type followed by a payload.
//
// Spawns a thread that owns the stream. The stream is used exclusively for
// MCP and must not also be a CLI console.
void mcp_uart_create(struct stream *s);
