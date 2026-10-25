// Host mode driver for the Synopsys DWC2 OTG core, using buffer DMA.
//
// In DMA mode the core retries NAKed transactions by itself and only
// raises "channel halted" once a transfer is done or has failed, so each
// transfer costs a single interrupt.
//
// Included from platform code, which also sets up clocks and the PHY.
//
// Channel 0 is used for control transfers, which are synchronous. Bulk
// pipes get a channel each for as long as they are open.
//
// Only used on STM32N6 so far. Cores without internal DMA (OTG_FS on
// STM32F4 etc) are not supported, they would need a slave mode path
// where the CPU retries every NAK. Using this on STM32H7 OTG_HS would
// need:
//
//  - The GCCFG/GOTGCTL setup in otgh_core_init() moved to a platform
//    hook. Bits 23 and 25 of GCCFG are N6 specific, on H7 bit 16
//    (PWRDWN) powers up the FS transceiver instead.
//  - FIFO RAM size (4 KB here) and channel count (16 here) passed in
//    by the platform.
//  - FSLSPCS in HCFG set after port reset. H7 has no internal HS PHY,
//    with the embedded FS PHY it must select the 48 MHz (FS) or 6 MHz
//    (LS) clock, followed by another port reset. The N6 HS PHY works
//    with the default.
//  - DMA buffers in memory the OTG_HS master can reach (not DTCM).
//  - A stm32h7_usbh.c with base, IRQ, clocks and VBUS, like
//    stm32n6_usbh.c.

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <malloc.h>
#include <sys/param.h>

#include <mios/io.h>
#include <mios/task.h>
#include <mios/device.h>
#include <mios/error.h>
#include <mios/stream.h>

#include <usb/usb_host.h>

#include "irq.h"
#include "cache.h"

#define OTGH_GOTGCTL     0x000
#define OTGH_GAHBCFG     0x008
#define OTGH_GUSBCFG     0x00c
#define OTGH_GRSTCTL     0x010
#define OTGH_GINTSTS     0x014
#define OTGH_GINTMSK     0x018
#define OTGH_GRXFSIZ     0x024
#define OTGH_HNPTXFSIZ   0x028
#define OTGH_GCCFG       0x038
#define OTGH_HPTXFSIZ    0x100
#define OTGH_HCFG        0x400
#define OTGH_HFNUM       0x408
#define OTGH_HAINT       0x414
#define OTGH_HAINTMSK    0x418
#define OTGH_HPRT        0x440
#define OTGH_HCCHAR(x)   (0x500 + 0x20 * (x))
#define OTGH_HCINT(x)    (0x508 + 0x20 * (x))
#define OTGH_HCINTMSK(x) (0x50c + 0x20 * (x))
#define OTGH_HCTSIZ(x)   (0x510 + 0x20 * (x))
#define OTGH_HCDMA(x)    (0x514 + 0x20 * (x))
#define OTGH_PCGCCTL     0xe00

#define GINT_HPRTINT  (1 << 24)
#define GINT_HCINT    (1 << 25)
#define GINT_DISCINT  (1 << 29)

#define HPRT_PCSTS    (1 << 0)
#define HPRT_PCDET    (1 << 1)
#define HPRT_PENA     (1 << 2)
#define HPRT_PENCHNG  (1 << 3)
#define HPRT_POCCHNG  (1 << 5)
#define HPRT_PRST     (1 << 8)
#define HPRT_PPWR     (1 << 12)

// Bits cleared by writing 1. Must be written as 0 in read-modify-write,
// writing back PENA would disable the port
#define HPRT_W1C (HPRT_PCDET | HPRT_PENA | HPRT_PENCHNG | HPRT_POCCHNG)

#define HCCHAR_CHENA  (1 << 31)
#define HCCHAR_CHDIS  (1 << 30)
#define HCCHAR_LSDEV  (1 << 17)
#define HCCHAR_EPDIR  (1 << 15)

#define HCINT_XFRC    (1 << 0)
#define HCINT_CHH     (1 << 1)
#define HCINT_AHBERR  (1 << 2)
#define HCINT_STALL   (1 << 3)
#define HCINT_TXERR   (1 << 7)
#define HCINT_BBERR   (1 << 8)
#define HCINT_DTERR   (1 << 10)

#define PID_DATA0 0
#define PID_DATA1 2
#define PID_SETUP 3

#define OTGH_NUM_CHANNELS 16

// Control transfers go through this buffer: SETUP packet first, data
// stage at OTGH_BOUNCE_DATA
#define OTGH_BOUNCE_SIZE  1024
#define OTGH_BOUNCE_DATA  64

#define PORT_EV_CONNECT    0x1
#define PORT_EV_CHANGE     0x2

typedef struct otgh_chan {
  task_waitable_t oc_wait;
  uint32_t oc_hcint;  // Accumulated HCINT bits
  uint8_t oc_done;    // Channel has halted
} otgh_chan_t;


struct usb_host_pipe {
  uint32_t up_hcchar;        // Address, endpoint, direction, type, MPS
  uint32_t up_xfrsiz;        // Transfer size as programmed
  usb_host_xfer_t *up_xfer;  // Transfer in flight
  uint8_t up_ch;
  uint8_t up_pid;            // Data toggle for the next transfer
  uint8_t up_closing;
};


typedef struct otgh {
  usb_host_ctrl_t oh_uhc;

  uint32_t oh_base;
  int oh_irq;
  gpio_t oh_vbus;

  mutex_t oh_ctrl_mutex;  // Serializes control transfers (on channel 0)
  uint8_t *oh_bounce;

  task_waitable_t oh_port_wait;
  task_waitable_t oh_state_wait;
  uint8_t oh_enabled;     // Requested state
  uint8_t oh_running;     // The thread owns the core
  uint8_t oh_port_events;
  uint32_t oh_disconnects;

  otgh_chan_t oh_chan[OTGH_NUM_CHANNELS];
  usb_host_pipe_t *oh_pipes[OTGH_NUM_CHANNELS];

  uint32_t oh_port_resets;
  uint32_t oh_overcurrents;
  uint32_t oh_stalls;
  uint32_t oh_xact_errors;
  uint32_t oh_timeouts;
} otgh_t;


static inline uint32_t
oh_rd(otgh_t *oh, uint32_t reg)
{
  return reg_rd(oh->oh_base + reg);
}

static inline void
oh_wr(otgh_t *oh, uint32_t reg, uint32_t value)
{
  reg_wr(oh->oh_base + reg, value);
}


static void
hprt_modify(otgh_t *oh, uint32_t clr, uint32_t set)
{
  int q = irq_forbid(IRQ_LEVEL_IO);
  const uint32_t v = oh_rd(oh, OTGH_HPRT) & ~HPRT_W1C;
  oh_wr(oh, OTGH_HPRT, (v & ~clr) | set);
  irq_permit(q);
}


static int
otgh_hcint_error(otgh_t *oh, uint32_t hcint)
{
  if(hcint & HCINT_STALL) {
    oh->oh_stalls++;
    return ERR_OPERATION_FAILED;
  }
  if(hcint & HCINT_TXERR) {
    oh->oh_xact_errors++;
    return ERR_IO;
  }
  if(hcint & HCINT_BBERR)
    return ERR_BAD_PKT_SIZE;
  if(hcint & HCINT_DTERR)
    return ERR_CHECKSUM_ERROR;
  if(hcint & HCINT_AHBERR)
    return ERR_DMA_XFER;
  // Halted for no other reason means we did it, due to disconnect
  return ERR_NOT_CONNECTED;
}


// Program and enable a channel. Called with IRQ_LEVEL_IO blocked.
// Returns the programmed transfer size
static uint32_t
otgh_chan_start(otgh_t *oh, int ch, uint32_t hcchar, int pid,
                void *buf, size_t len, int is_in)
{
  otgh_chan_t *oc = &oh->oh_chan[ch];
  const uint32_t mps = hcchar & 0x7ff;
  const uint32_t pktcnt = len ? (len + mps - 1) / mps : 1;
  // IN transfers must be programmed as a multiple of the packet size,
  // a short packet ends the transfer early
  const uint32_t xfrsiz = is_in ? pktcnt * mps : len;

  if(is_in)
    hcchar |= HCCHAR_EPDIR;

  oc->oc_hcint = 0;
  oc->oc_done = 0;

  oh_wr(oh, OTGH_HCINT(ch), 0x7ff);
  oh_wr(oh, OTGH_HCTSIZ(ch), (pid << 29) | (pktcnt << 19) | xfrsiz);
  oh_wr(oh, OTGH_HCDMA(ch), (uint32_t)buf);
  asm volatile ("dsb"); // Make sure the core sees what we wrote to buf
  oh_wr(oh, OTGH_HCCHAR(ch), hcchar | HCCHAR_CHENA);
  return xfrsiz;
}


static void
otgh_pipe_halted(otgh_t *oh, usb_host_pipe_t *up, uint32_t hcint)
{
  usb_host_xfer_t *ux = up->up_xfer;
  if(ux == NULL)
    return;
  up->up_xfer = NULL;

  const uint32_t hctsiz = oh_rd(oh, OTGH_HCTSIZ(up->up_ch));
  up->up_pid = (hctsiz >> 29) & 3;

  if(hcint & HCINT_XFRC) {
    if(up->up_hcchar & HCCHAR_EPDIR) {
      ux->ux_result = up->up_xfrsiz - (hctsiz & 0x7ffff);
      dcache_op(ux->ux_buf, ux->ux_len, DCACHE_INVALIDATE);
    } else {
      ux->ux_result = ux->ux_len;
    }
  } else if(up->up_closing) {
    ux->ux_result = ERR_INTERRUPTED;
  } else {
    ux->ux_result = otgh_hcint_error(oh, hcint);
  }
  ux->ux_done(ux);
}


static void
otgh_irq(void *arg)
{
  otgh_t *oh = arg;
  const uint32_t gint = oh_rd(oh, OTGH_GINTSTS) & oh_rd(oh, OTGH_GINTMSK);

  if(gint & GINT_HPRTINT) {
    const uint32_t hprt = oh_rd(oh, OTGH_HPRT);
    // Writing back the change bits acks them
    oh_wr(oh, OTGH_HPRT, hprt & ~HPRT_PENA);

    if(hprt & HPRT_PCDET)
      oh->oh_port_events |= PORT_EV_CONNECT;
    if(hprt & HPRT_PENCHNG)
      oh->oh_port_events |= PORT_EV_CHANGE;
    if(hprt & HPRT_POCCHNG) {
      oh->oh_port_events |= PORT_EV_CHANGE;
      oh->oh_overcurrents++;
    }
    task_wakeup(&oh->oh_port_wait, 1);
  }

  if(gint & GINT_DISCINT) {
    oh_wr(oh, OTGH_GINTSTS, GINT_DISCINT);
    oh->oh_disconnects++;
    oh->oh_port_events |= PORT_EV_CHANGE;
    task_wakeup(&oh->oh_port_wait, 1);

    // Halt any transfer in flight, it completes with CHH but no XFRC
    for(int i = 0; i < OTGH_NUM_CHANNELS; i++) {
      const uint32_t hcchar = oh_rd(oh, OTGH_HCCHAR(i));
      if(hcchar & HCCHAR_CHENA)
        oh_wr(oh, OTGH_HCCHAR(i), hcchar | HCCHAR_CHDIS);
    }
  }

  if(gint & GINT_HCINT) {
    const uint32_t haint = oh_rd(oh, OTGH_HAINT);
    for(int i = 0; i < OTGH_NUM_CHANNELS; i++) {
      if(!(haint & (1 << i)))
        continue;
      otgh_chan_t *oc = &oh->oh_chan[i];
      const uint32_t hcint = oh_rd(oh, OTGH_HCINT(i));
      oh_wr(oh, OTGH_HCINT(i), hcint);
      oc->oc_hcint |= hcint;
      if(!(hcint & HCINT_CHH))
        continue;
      if(oh->oh_pipes[i] != NULL)
        otgh_pipe_halted(oh, oh->oh_pipes[i], oc->oc_hcint);
      else
        oc->oc_done = 1;
      task_wakeup(&oc->oc_wait, 0);
    }
  }
}


// Run one control transfer stage on channel 0 and wait for it to halt.
// Returns number of bytes transferred or an error
static int
otgh_xfer(otgh_t *oh, uint32_t hcchar, int pid,
          void *buf, size_t len, int is_in, int64_t deadline)
{
  otgh_chan_t *oc = &oh->oh_chan[0];

  // buf is within the bounce buffer, which is cache line aligned
  dcache_op(oh->oh_bounce, OTGH_BOUNCE_SIZE, DCACHE_CLEAN | DCACHE_INVALIDATE);

  int q = irq_forbid(IRQ_LEVEL_IO);

  const uint32_t xfrsiz = otgh_chan_start(oh, 0, hcchar, pid, buf, len,
                                          is_in);

  while(!oc->oc_done) {
    if(task_sleep_deadline(&oc->oc_wait, deadline)) {
      // Halt the channel. The core finishes the current transaction
      // first, so give it a while
      oh_wr(oh, OTGH_HCCHAR(0),
            oh_rd(oh, OTGH_HCCHAR(0)) | HCCHAR_CHENA | HCCHAR_CHDIS);
      const int64_t halt_deadline = clock_get() + 10000;
      while(!oc->oc_done) {
        if(task_sleep_deadline(&oc->oc_wait, halt_deadline))
          break;
      }
      oh->oh_timeouts++;
      irq_permit(q);
      return ERR_TIMEOUT;
    }
  }
  const uint32_t hcint = oc->oc_hcint;
  irq_permit(q);

  if(hcint & HCINT_XFRC) {
    if(!is_in)
      return len;
    dcache_op(oh->oh_bounce, OTGH_BOUNCE_SIZE, DCACHE_INVALIDATE);
    const uint32_t remain = oh_rd(oh, OTGH_HCTSIZ(0)) & 0x7ffff;
    return xfrsiz - remain;
  }
  return otgh_hcint_error(oh, hcint);
}


static int
otgh_control_once(otgh_t *oh, uint32_t hcchar,
                  const struct usb_setup_packet *setup, void *data,
                  int64_t deadline)
{
  uint8_t *const setup_buf = oh->oh_bounce;
  uint8_t *const data_buf = oh->oh_bounce + OTGH_BOUNCE_DATA;
  const size_t len = setup->length;
  const int data_in = setup->request_type & 0x80;
  int actual = 0;

  memcpy(setup_buf, setup, sizeof(struct usb_setup_packet));
  int r = otgh_xfer(oh, hcchar, PID_SETUP, setup_buf, 8, 0, deadline);
  if(r < 0)
    return r;

  if(len) {
    if(!data_in)
      memcpy(data_buf, data, len);
    r = otgh_xfer(oh, hcchar, PID_DATA1, data_buf, len, data_in,
                  deadline);
    if(r < 0)
      return r;
    actual = MIN(r, len);
    if(data_in)
      memcpy(data, data_buf, actual);
  }

  // Status stage goes in the opposite direction of the data stage,
  // IN if there was no data stage
  r = otgh_xfer(oh, hcchar, PID_DATA1, data_buf, 0, !(len && data_in),
                deadline);
  if(r < 0)
    return r;
  return actual;
}


static int
otgh_control(usb_host_ctrl_t *uhc, uint8_t addr, uint8_t speed,
             uint16_t mps0, const struct usb_setup_packet *setup,
             void *data)
{
  otgh_t *oh = (otgh_t *)uhc;

  if(((setup->length + mps0 - 1) / mps0) * mps0 >
     OTGH_BOUNCE_SIZE - OTGH_BOUNCE_DATA)
    return ERR_MTU_EXCEEDED;

  const uint32_t hcchar =
    (addr << 22) |
    (USB_ENDPOINT_CONTROL << 18) |
    (speed == USB_SPEED_LOW ? HCCHAR_LSDEV : 0) |
    mps0;

  const int64_t deadline = clock_get() + 1000000;

  mutex_lock(&oh->oh_ctrl_mutex);
  int r;
  // Retry on transaction errors (CRC, timeouts, etc)
  for(int i = 0; i < 3; i++) {
    r = otgh_control_once(oh, hcchar, setup, data, deadline);
    if(r != ERR_IO)
      break;
  }
  mutex_unlock(&oh->oh_ctrl_mutex);
  return r;
}


static usb_host_pipe_t *
otgh_pipe_open(usb_host_ctrl_t *uhc, uint8_t addr, uint8_t speed,
               const struct usb_endpoint_descriptor *ed)
{
  otgh_t *oh = (otgh_t *)uhc;
  const int type = ed->bmAttributes & 3;

  if(type != USB_ENDPOINT_BULK)
    return NULL; // Periodic transfers are not supported yet

  usb_host_pipe_t *up = calloc(1, sizeof(usb_host_pipe_t));
  up->up_hcchar =
    (addr << 22) |
    (type << 18) |
    (speed == USB_SPEED_LOW ? HCCHAR_LSDEV : 0) |
    (ed->bEndpointAddress & 0x80 ? HCCHAR_EPDIR : 0) |
    ((ed->bEndpointAddress & 0xf) << 11) |
    (ed->wMaxPacketSize & 0x7ff);
  up->up_pid = PID_DATA0;

  int q = irq_forbid(IRQ_LEVEL_IO);
  for(int i = 1; i < OTGH_NUM_CHANNELS; i++) {
    if(oh->oh_pipes[i] == NULL) {
      up->up_ch = i;
      oh->oh_pipes[i] = up;
      break;
    }
  }
  irq_permit(q);

  if(up->up_ch == 0) {
    free(up);
    return NULL;
  }
  return up;
}


static void
otgh_pipe_close(usb_host_ctrl_t *uhc, usb_host_pipe_t *up)
{
  otgh_t *oh = (otgh_t *)uhc;
  otgh_chan_t *oc = &oh->oh_chan[up->up_ch];

  int q = irq_forbid(IRQ_LEVEL_IO);
  up->up_closing = 1;

  if(up->up_xfer != NULL) {
    const uint32_t hcchar = oh_rd(oh, OTGH_HCCHAR(up->up_ch));
    if(hcchar & HCCHAR_CHENA)
      oh_wr(oh, OTGH_HCCHAR(up->up_ch), hcchar | HCCHAR_CHDIS);

    const int64_t deadline = clock_get() + 100000;
    while(up->up_xfer != NULL) {
      if(task_sleep_deadline(&oc->oc_wait, deadline))
        break;
    }

    if(up->up_xfer != NULL) {
      // Channel never halted, complete the transfer ourselves
      usb_host_xfer_t *ux = up->up_xfer;
      up->up_xfer = NULL;
      ux->ux_result = ERR_INTERRUPTED;
      ux->ux_done(ux);
    }
  }
  oh->oh_pipes[up->up_ch] = NULL;
  irq_permit(q);
  free(up);
}


static error_t
otgh_submit(usb_host_ctrl_t *uhc, usb_host_pipe_t *up, usb_host_xfer_t *ux)
{
  otgh_t *oh = (otgh_t *)uhc;
  const int is_in = !!(up->up_hcchar & HCCHAR_EPDIR);
  error_t err = 0;

  int q = irq_forbid(IRQ_LEVEL_IO);

  if(up->up_closing || !(oh_rd(oh, OTGH_HPRT) & HPRT_PENA)) {
    err = ERR_NOT_CONNECTED;
  } else if(up->up_xfer != NULL) {
    err = ERR_NOT_IDLE;
  } else {
    dcache_op(ux->ux_buf, ux->ux_len,
              is_in ? DCACHE_INVALIDATE : DCACHE_CLEAN);
    up->up_xfer = ux;
    up->up_xfrsiz = otgh_chan_start(oh, up->up_ch, up->up_hcchar,
                                    up->up_pid, ux->ux_buf, ux->ux_len,
                                    is_in);
  }
  irq_permit(q);
  return err;
}


static const usb_host_ctrl_vtable_t otgh_vtable = {
  .control = otgh_control,
  .pipe_open = otgh_pipe_open,
  .pipe_close = otgh_pipe_close,
  .submit = otgh_submit,
};


static error_t
otgh_wait_grstctl(otgh_t *oh, uint32_t mask, uint32_t value)
{
  for(int i = 0; i < 1000; i++) {
    if((oh_rd(oh, OTGH_GRSTCTL) & mask) == value)
      return 0;
    udelay(10);
  }
  return ERR_TIMEOUT;
}


static void
otgh_core_init(otgh_t *oh)
{
  // Core soft reset, AHB master must be idle first
  if(otgh_wait_grstctl(oh, 1 << 31, 1 << 31))
    printf("%s: AHB not idle\n", oh->oh_uhc.uhc_dev.d_name);
  oh_wr(oh, OTGH_GRSTCTL, 1);
  if(otgh_wait_grstctl(oh, 1, 0))
    printf("%s: Core reset timeout\n", oh->oh_uhc.uhc_dev.d_name);
  udelay(10);

  // Force host mode, takes 25ms to have effect
  oh_wr(oh, OTGH_GUSBCFG,
        (oh_rd(oh, OTGH_GUSBCFG) & ~(1 << 30)) | (1 << 29));
  usleep(50000);

  oh_wr(oh, OTGH_PCGCCTL, 0);

  // No VBUS sensing, override VBUS valid and A-session valid
  oh_wr(oh, OTGH_GOTGCTL, oh_rd(oh, OTGH_GOTGCTL) | 0x3c);
  // Host mode pull-downs on DP/DM (no ID pin) and VBUS valid
  oh_wr(oh, OTGH_GCCFG, oh_rd(oh, OTGH_GCCFG) | (1 << 25) | (1 << 23));

  // 1024 words of FIFO RAM. Leave the top 128 words unused, in DMA mode
  // the core keeps per-channel state at the end of the RAM
  oh_wr(oh, OTGH_GRXFSIZ, 384);
  oh_wr(oh, OTGH_HNPTXFSIZ, (256 << 16) | 384);
  oh_wr(oh, OTGH_HPTXFSIZ, (256 << 16) | 640);

  oh_wr(oh, OTGH_GRSTCTL, (0x10 << 6) | (1 << 5)); // Flush all TX FIFOs
  otgh_wait_grstctl(oh, 1 << 5, 0);
  oh_wr(oh, OTGH_GRSTCTL, 1 << 4);                 // Flush RX FIFO
  otgh_wait_grstctl(oh, 1 << 4, 0);

  for(int i = 0; i < OTGH_NUM_CHANNELS; i++)
    oh_wr(oh, OTGH_HCINT(i), 0x7ff);

  // In DMA mode channel halted is the only interrupt we need
  for(int i = 0; i < OTGH_NUM_CHANNELS; i++)
    oh_wr(oh, OTGH_HCINTMSK(i), HCINT_CHH);
  oh_wr(oh, OTGH_HAINTMSK, (1 << OTGH_NUM_CHANNELS) - 1);

  oh_wr(oh, OTGH_GINTSTS, 0xffffffff);
  oh_wr(oh, OTGH_GINTMSK, GINT_HPRTINT | GINT_HCINT | GINT_DISCINT);

  oh_wr(oh, OTGH_GAHBCFG,
        (1 << 5) |  // DMA enable
        (3 << 1) |  // INCR4 bursts
        (1 << 0));  // Global interrupt enable
}


// Wait for port events (or until deadline, 0 means forever).
// Returns and clears the pending events
static int
otgh_port_wait(otgh_t *oh, int64_t deadline)
{
  int q = irq_forbid(IRQ_LEVEL_IO);
  while(!oh->oh_port_events) {
    if(deadline) {
      if(task_sleep_deadline(&oh->oh_port_wait, deadline))
        break;
    } else {
      task_sleep(&oh->oh_port_wait);
    }
  }
  const int events = oh->oh_port_events;
  oh->oh_port_events = 0;
  irq_permit(q);
  return events;
}


// Reset the port, returns the speed of the attached device or an error
static int
otgh_port_reset(otgh_t *oh)
{
  oh->oh_port_resets++;
  hprt_modify(oh, 0, HPRT_PRST);
  usleep(50000); // Root port reset is at least 50ms
  hprt_modify(oh, HPRT_PRST, 0);

  for(int i = 0; i < 100; i++) {
    const uint32_t hprt = oh_rd(oh, OTGH_HPRT);
    if(!(hprt & HPRT_PCSTS))
      return ERR_NOT_CONNECTED;
    if(hprt & HPRT_PENA) {
      usleep(10000); // Reset recovery
      const int speed = (hprt >> 17) & 3;
      return speed == 3 ? ERR_BAD_STATE : speed;
    }
    usleep(1000);
  }
  return ERR_TIMEOUT;
}


static void
otgh_start(otgh_t *oh)
{
  otgh_core_init(oh);
  irq_enable_fn_arg(oh->oh_irq, IRQ_LEVEL_IO, otgh_irq, oh);

  hprt_modify(oh, 0, HPRT_PPWR);
  if(oh->oh_vbus != GPIO_UNUSED) {
    gpio_conf_output(oh->oh_vbus, GPIO_PUSH_PULL, GPIO_SPEED_LOW,
                     GPIO_PULL_NONE);
    gpio_set_output(oh->oh_vbus, 1);
  }
}


// Hand the core back in the state we found it, so device mode can use it
static void
otgh_stop(otgh_t *oh)
{
  // Detach while interrupts still work, closing pipes halts channels
  usb_host_detach(&oh->oh_uhc);

  if(oh->oh_vbus != GPIO_UNUSED)
    gpio_set_output(oh->oh_vbus, 0);
  hprt_modify(oh, HPRT_PPWR, 0);

  irq_disable(oh->oh_irq);
  oh_wr(oh, OTGH_GINTMSK, 0);
  oh_wr(oh, OTGH_GAHBCFG, 0);

  otgh_wait_grstctl(oh, 1 << 31, 1 << 31);
  oh_wr(oh, OTGH_GRSTCTL, 1);
  otgh_wait_grstctl(oh, 1, 0);

  oh_wr(oh, OTGH_GUSBCFG, oh_rd(oh, OTGH_GUSBCFG) & ~(1 << 29));
  oh_wr(oh, OTGH_GOTGCTL, oh_rd(oh, OTGH_GOTGCTL) & ~0x3c);
  oh_wr(oh, OTGH_GCCFG, oh_rd(oh, OTGH_GCCFG) & ~(1 << 25));
}


// Handle devices on the port until disabled
static void
otgh_run(otgh_t *oh)
{
  const char *name = oh->oh_uhc.uhc_dev.d_name;

  while(oh->oh_enabled) {
    if(!(oh_rd(oh, OTGH_HPRT) & HPRT_PCSTS)) {
      otgh_port_wait(oh, 0);
      continue;
    }

    // Let the connection settle (USB 2.0 spec 7.1.7.3)
    usleep(100000);
    if(!(oh_rd(oh, OTGH_HPRT) & HPRT_PCSTS) || !oh->oh_enabled)
      continue;

    const uint32_t disconnects = oh->oh_disconnects;
    const int speed = otgh_port_reset(oh);
    if(speed < 0) {
      printf("%s: Port reset failed: %s\n", name, error_to_string(speed));
      // Retry after a while, or sooner if something happens on the port
      otgh_port_wait(oh, clock_get() + 1000000);
      continue;
    }

    usb_host_attach(&oh->oh_uhc, speed);

    while((oh_rd(oh, OTGH_HPRT) & HPRT_PCSTS) &&
          oh->oh_disconnects == disconnects && oh->oh_enabled) {
      otgh_port_wait(oh, 0);
    }

    usb_host_detach(&oh->oh_uhc);
  }
}


__attribute__((noreturn))
static void *
otgh_thread(void *arg)
{
  otgh_t *oh = arg;

  while(1) {
    int q = irq_forbid(IRQ_LEVEL_IO);
    while(!oh->oh_enabled)
      task_sleep(&oh->oh_port_wait);
    oh->oh_running = 1;
    irq_permit(q);

    otgh_start(oh);
    otgh_run(oh);
    otgh_stop(oh);

    q = irq_forbid(IRQ_LEVEL_IO);
    oh->oh_running = 0;
    task_wakeup(&oh->oh_state_wait, 1);
    irq_permit(q);
  }
}


// Turn host mode on or off. Turning it off waits until the core has been
// released. Thread context
__attribute__((unused))
static void
stm32_otg_host_enable(otgh_t *oh, int on)
{
  int q = irq_forbid(IRQ_LEVEL_IO);
  oh->oh_enabled = on;
  oh->oh_port_events |= PORT_EV_CHANGE;
  task_wakeup(&oh->oh_port_wait, 1);
  if(!on) {
    while(oh->oh_running)
      task_sleep(&oh->oh_state_wait);
  }
  irq_permit(q);
}


static void
otgh_print_info(struct device *d, struct stream *st)
{
  otgh_t *oh = (otgh_t *)d;
  static const char *speeds[4] = {"high", "full", "low", "?"};

  if(!oh->oh_running) {
    stprintf(st, "Disabled\n");
    return;
  }

  const uint32_t hprt = oh_rd(oh, OTGH_HPRT);
  stprintf(st, "Port: %s%s%s%s%s  Line D+:%d D-:%d\n",
           hprt & HPRT_PPWR ? "powered" : "unpowered",
           hprt & HPRT_PCSTS ? ", connected" : "",
           hprt & HPRT_PENA ? ", enabled " : "",
           hprt & HPRT_PENA ? speeds[(hprt >> 17) & 3] : "",
           hprt & HPRT_PENA ? " speed" : "",
           (hprt >> 10) & 1, (hprt >> 11) & 1);
  stprintf(st, "Frame: %d\n", oh_rd(oh, OTGH_HFNUM) & 0xffff);
  stprintf(st, "Port resets: %d  Disconnects: %d  Overcurrents: %d\n",
           oh->oh_port_resets, oh->oh_disconnects, oh->oh_overcurrents);
  stprintf(st, "Stalls: %d  Transaction errors: %d  Timeouts: %d\n",
           oh->oh_stalls, oh->oh_xact_errors, oh->oh_timeouts);

  for(int i = 1; i < OTGH_NUM_CHANNELS; i++) {
    const usb_host_pipe_t *up = oh->oh_pipes[i];
    if(up == NULL)
      continue;
    stprintf(st, "Channel %2d: addr %d EP 0x%02x%s\n", i,
             (up->up_hcchar >> 22) & 0x7f,
             ((up->up_hcchar >> 11) & 0xf) |
             (up->up_hcchar & HCCHAR_EPDIR ? 0x80 : 0),
             up->up_xfer ? " busy" : "");
  }
}


static const device_class_t otgh_class = {
  .dc_class_name = "usb-host",
  .dc_print_info = otgh_print_info,
};


static otgh_t *
stm32_otg_host_create(uint32_t base, int irq, gpio_t vbus, const char *name,
                      int enabled)
{
  otgh_t *oh = calloc(1, sizeof(otgh_t));
  oh->oh_base = base;
  oh->oh_vbus = vbus;
  oh->oh_uhc.uhc_vtable = &otgh_vtable;
  oh->oh_bounce = xalloc(OTGH_BOUNCE_SIZE, 64, MEM_TYPE_DMA);
  oh->oh_irq = irq;
  mutex_init(&oh->oh_ctrl_mutex, "usbhctrl");

  oh->oh_uhc.uhc_dev.d_name = name;
  oh->oh_uhc.uhc_dev.d_class = &otgh_class;
  device_register(&oh->oh_uhc.uhc_dev);

  // Core init sleeps (forcing host mode takes 25ms), so do it in the thread
  oh->oh_enabled = enabled;
  thread_create(otgh_thread, oh, 2048, name, 0, 4);
  return oh;
}
