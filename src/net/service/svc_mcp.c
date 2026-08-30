#include <mios/service.h>
#include <mios/stream.h>
#include <mios/mcp.h>

// Runs the same MCP protocol already used over USB (usb_mcp.c) and UART
// (mcp_uart.c) -- see docs/mcp.md -- over a VLLP channel instead, so
// mcp__mios-style tooling can reach a board without a direct USB
// connection to it (e.g. relayed over CAN/dsig through another board's
// USB link).
static error_t
mcp_vllp_open(stream_t *s)
{
  mcp_uart_create(s);
  return 0;
}

SERVICE_DEF_STREAM("mcp", 0, mcp_vllp_open);
