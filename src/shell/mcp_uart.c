#include <mios/mcp.h>
#include <mios/stream.h>
#include <mios/task.h>
#include <mios/cli.h>

#include <string.h>
#include <stdlib.h>

#include "util/hdlc.h"

// Protocol message types (shared with the USB MCP interface, see docs/mcp.md)
#define MCP_CLI_EXECUTE     0x01
#define MCP_CLI_RESPONSE    0x02
#define MCP_CLI_COMPLETE    0x03
#define MCP_MEM_READ        0x04
#define MCP_MEM_READ_RESP   0x05

// HDLC frames are arbitrary length, so we are not limited to USB's 64 bytes.
#define MCP_MAX_FRAME       256
#define MCP_MAX_MEM_READ    (MCP_MAX_FRAME - 1)

typedef struct mcp_uart {

  stream_t stream;       // CLI output capture stream

  stream_t *uart;        // underlying HDLC-framed byte stream

  uint16_t txoff;        // fill level of txbuf (byte 0 = message type)

  uint8_t txbuf[MCP_MAX_FRAME];
  uint8_t rxbuf[MCP_MAX_FRAME + 1]; // +1 so we can NUL-terminate a full frame

} mcp_uart_t;


// Send the currently accumulated CLI output as one MCP_CLI_RESPONSE frame
static void
mcp_flush_output(mcp_uart_t *m)
{
  if(m->txoff > 1) {
    hdlc_send(m->uart, m->txbuf, m->txoff);
    m->txoff = 1;
  }
}


// Capture stream: cli_dispatch() writes command output here
static ssize_t
mcp_stream_write(stream_t *s, const void *buf, size_t size, int flags)
{
  mcp_uart_t *m = (mcp_uart_t *)s;

  if(buf == NULL && size == 0) {
    mcp_flush_output(m);
    return 0;
  }

  const uint8_t *src = buf;
  size_t written = 0;

  while(written < size) {
    size_t space = MCP_MAX_FRAME - m->txoff;
    size_t chunk = size - written;
    if(chunk > space)
      chunk = space;

    memcpy(m->txbuf + m->txoff, src + written, chunk);
    m->txoff += chunk;
    written += chunk;

    if(m->txoff == MCP_MAX_FRAME)
      mcp_flush_output(m);
  }

  return written;
}

static const stream_vtable_t mcp_stream_vtable = {
  .write = mcp_stream_write,
};


static void
mcp_handle(mcp_uart_t *m, uint8_t *cmd, int len)
{
  if(len < 1)
    return;

  switch(cmd[0]) {
  case MCP_CLI_EXECUTE: {
    cli_t cli = { .cl_stream = &m->stream };

    cmd[len] = '\0'; // rxbuf has room for this
    char *cmdline = (char *)(cmd + 1);

    m->txbuf[0] = MCP_CLI_RESPONSE;
    m->txoff = 1;

    error_t err = cli_dispatch(&cli, cmdline);
    mcp_flush_output(m);

    int32_t err32 = err;
    uint8_t pkt[5];
    pkt[0] = MCP_CLI_COMPLETE;
    memcpy(pkt + 1, &err32, sizeof(err32));
    hdlc_send(m->uart, pkt, sizeof(pkt));
    break;
  }

  case MCP_MEM_READ: {
    if(len < 9)
      break;
    uint32_t addr, length;
    memcpy(&addr, cmd + 1, 4);
    memcpy(&length, cmd + 5, 4);
    if(length > MCP_MAX_MEM_READ)
      length = MCP_MAX_MEM_READ;

    m->txbuf[0] = MCP_MEM_READ_RESP;
    memcpy(m->txbuf + 1, (const void *)(uintptr_t)addr, length);
    hdlc_send(m->uart, m->txbuf, 1 + length);
    break;
  }
  }
}


__attribute__((noreturn))
static void *
mcp_uart_thread(void *arg)
{
  mcp_uart_t *m = arg;

  while(1) {
    int len = hdlc_read_to_buf(m->uart, m->rxbuf, MCP_MAX_FRAME, 1);
    if(len > 0)
      mcp_handle(m, m->rxbuf, len);
  }
}


void
mcp_uart_create(stream_t *s)
{
  mcp_uart_t *m = calloc(1, sizeof(mcp_uart_t));
  m->stream.vtable = &mcp_stream_vtable;
  m->uart = s;
  m->txoff = 1;

  thread_create_shell(mcp_uart_thread, m, "mcp-uart", &m->stream);
}
