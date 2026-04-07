#include <mios/stream.h>
#include <mios/task.h>
#include <mios/cli.h>

#include <string.h>
#include <stdlib.h>

#include <usb/usb_desc.h>
#include <usb/usb.h>

#include "irq.h"

// Protocol message types
#define MCP_CLI_EXECUTE     0x01
#define MCP_CLI_RESPONSE    0x02
#define MCP_CLI_COMPLETE    0x03
#define MCP_MEM_READ        0x04
#define MCP_MEM_READ_RESP   0x05

#define MCP_MAX_MEM_READ    32

typedef struct usb_mcp {

  stream_t stream;

  uint8_t usb_sub_class;
  usb_interface_t *iface;

  // RX: pending command from host (set in IRQ, consumed by thread)
  uint8_t rx_buf[64];
  uint8_t rx_len;
  uint8_t rx_pending;

  // TX: output packet being built
  uint8_t tx_buf[64];
  uint8_t tx_offset;  // Next write pos (byte 0 = type, data starts at 1)

  // TX flow control
  task_waitable_t tx_waitq;
  uint8_t tx_busy;

  // Thread wakeup
  task_waitable_t rx_waitq;

} usb_mcp_t;


static size_t
mcp_gen_desc(void *ptr, void *opaque, int iface_index)
{
  usb_mcp_t *um = opaque;
  return usb_gen_iface_desc(ptr, iface_index, 2, 255, um->usb_sub_class);
}


// --- TX path ---

static void
mcp_send_packet(usb_mcp_t *um, const void *data, size_t len)
{
  usb_ep_t *ue = &um->iface->ui_endpoints[1]; // IN

  int q = irq_forbid(IRQ_LEVEL_NET);
  while(um->tx_busy || !ue->ue_running)
    task_sleep(&um->tx_waitq);

  ue->ue_vtable->write(ue->ue_dev, ue, data, len);
  um->tx_busy = 1;
  irq_permit(q);
}


static void
mcp_flush_output(usb_mcp_t *um)
{
  if(um->tx_offset > 1) {
    mcp_send_packet(um, um->tx_buf, um->tx_offset);
    um->tx_offset = 1;
  }
}


static void
mcp_txco(device_t *d, usb_ep_t *ue, uint32_t bytes, uint32_t flags)
{
  usb_mcp_t *um = ue->ue_iface_aux;
  um->tx_busy = 0;
  task_wakeup(&um->tx_waitq, 0);
}


static void
mcp_tx_reset(device_t *d, usb_ep_t *ue)
{
  usb_mcp_t *um = ue->ue_iface_aux;
  task_wakeup(&um->tx_waitq, 0);
}


// --- Stream vtable for CLI output capture ---

static ssize_t
mcp_stream_write(stream_t *s, const void *buf, size_t size, int flags)
{
  usb_mcp_t *um = (usb_mcp_t *)s;
  (void)flags;

  if(buf == NULL && size == 0) {
    // Flush
    mcp_flush_output(um);
    return 0;
  }

  const uint8_t *src = buf;
  size_t written = 0;

  while(written < size) {
    size_t space = 64 - um->tx_offset;
    size_t chunk = size - written;
    if(chunk > space)
      chunk = space;

    memcpy(um->tx_buf + um->tx_offset, src + written, chunk);
    um->tx_offset += chunk;
    written += chunk;

    if(um->tx_offset == 64)
      mcp_flush_output(um);
  }

  return written;
}

static const stream_vtable_t mcp_stream_vtable = {
  .write = mcp_stream_write,
};


// --- RX path ---

static void
mcp_rx(device_t *d, usb_ep_t *ue, uint32_t bytes, uint32_t flags)
{
  usb_mcp_t *um = ue->ue_iface_aux;

  ue->ue_vtable->read(d, ue, um->rx_buf, sizeof(um->rx_buf), 0, bytes);
  um->rx_len = bytes;
  um->rx_pending = 1;
  task_wakeup(&um->rx_waitq, 0);
  // Don't CNAK — back-pressure until thread processes the command
}


// --- Thread ---

__attribute__((noreturn))
static void *
mcp_thread(void *arg)
{
  usb_mcp_t *um = arg;
  cli_t cli = { .cl_stream = &um->stream };

  while(1) {
    int q = irq_forbid(IRQ_LEVEL_NET);
    while(!um->rx_pending)
      task_sleep(&um->rx_waitq);

    uint8_t cmd[64];
    int len = um->rx_len;
    memcpy(cmd, um->rx_buf, len);
    um->rx_pending = 0;

    // CNAK the OUT endpoint — ready for next command
    usb_ep_t *ue_out = &um->iface->ui_endpoints[0];
    if(ue_out->ue_vtable)
      ue_out->ue_vtable->cnak(ue_out->ue_dev, ue_out);

    irq_permit(q);

    if(len < 1)
      continue;

    switch(cmd[0]) {
    case MCP_CLI_EXECUTE: {
      char cmdline[64];
      int cmdlen = len - 1;
      if(cmdlen > 63) cmdlen = 63;
      memcpy(cmdline, cmd + 1, cmdlen);
      cmdline[cmdlen] = '\0';

      um->tx_buf[0] = MCP_CLI_RESPONSE;
      um->tx_offset = 1;

      error_t err = cli_dispatch(&cli, cmdline);
      mcp_flush_output(um);

      // Send completion
      uint8_t pkt[5];
      pkt[0] = MCP_CLI_COMPLETE;
      memcpy(pkt + 1, &err, sizeof(err));
      mcp_send_packet(um, pkt, sizeof(pkt));
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

      uint8_t pkt[1 + MCP_MAX_MEM_READ];
      pkt[0] = MCP_MEM_READ_RESP;
      memcpy(pkt + 1, (const void *)(uintptr_t)addr, length);
      mcp_send_packet(um, pkt, 1 + length);
      break;
    }
    }
  }
}


void
usb_mcp_create(struct usb_interface_queue *q, uint8_t usb_sub_class)
{
  usb_mcp_t *um = calloc(1, sizeof(usb_mcp_t));
  um->stream.vtable = &mcp_stream_vtable;
  um->usb_sub_class = usb_sub_class;

  task_waitable_init(&um->tx_waitq, "mcptx");
  task_waitable_init(&um->rx_waitq, "mcprx");

  um->iface = usb_alloc_interface(q, mcp_gen_desc, um, 2, "usb-mcp");

  usb_init_endpoint(&um->iface->ui_endpoints[0],
                    um, mcp_rx, NULL,
                    USB_ENDPOINT_BULK, 0x0, 0x1, 64);

  usb_init_endpoint(&um->iface->ui_endpoints[1],
                    um, mcp_txco, mcp_tx_reset,
                    USB_ENDPOINT_BULK, 0x80, 0x1, 64);

  thread_create_shell(mcp_thread, um, "usb-mcp", &um->stream);
}
