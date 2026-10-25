// Host mode driver for the Synopsys DWC2 OTG core, using buffer DMA.
//
// In DMA mode the core retries NAKed transactions by itself and only
// raises "channel halted" once a transfer is done or has failed, so each
// transfer costs a single interrupt.
//
// Included from platform code, which also sets up clocks and the PHY.
// Only control transfers on channel 0 so far.

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
#include <mios/type_macros.h>

#include <usb/usb_host.h>

#include "irq.h"

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

// Control transfers go through this uncached buffer: SETUP packet first,
// data stage at OTGH_BOUNCE_DATA
#define OTGH_BOUNCE_SIZE  1024
#define OTGH_BOUNCE_DATA  64

#define PORT_EV_CONNECT    0x1
#define PORT_EV_CHANGE     0x2

typedef struct otgh_chan {
  task_waitable_t oc_wait;
  uint32_t oc_hcint;  // Accumulated HCINT bits
  uint8_t oc_done;    // Channel has halted
} otgh_chan_t;


typedef struct otgh {
  usb_host_ctrl_t oh_uhc;

  uint32_t oh_base;
  int oh_irq;
  gpio_t oh_vbus;

  mutex_t oh_ctrl_mutex;  // Serializes control transfers (on channel 0)
  uint8_t *oh_bounce;

  task_waitable_t oh_port_wait;
  uint8_t oh_port_events;
  uint32_t oh_disconnects;

  otgh_chan_t oh_chan[1];

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

    // Halt any transfer in flight, the waiter sees CHH without XFRC
    for(int i = 0; i < ARRAYSIZE(oh->oh_chan); i++) {
      const uint32_t hcchar = oh_rd(oh, OTGH_HCCHAR(i));
      if(hcchar & HCCHAR_CHENA)
        oh_wr(oh, OTGH_HCCHAR(i), hcchar | HCCHAR_CHDIS);
    }
  }

  if(gint & GINT_HCINT) {
    const uint32_t haint = oh_rd(oh, OTGH_HAINT);
    for(int i = 0; i < ARRAYSIZE(oh->oh_chan); i++) {
      if(!(haint & (1 << i)))
        continue;
      otgh_chan_t *oc = &oh->oh_chan[i];
      const uint32_t hcint = oh_rd(oh, OTGH_HCINT(i));
      oh_wr(oh, OTGH_HCINT(i), hcint);
      oc->oc_hcint |= hcint;
      if(hcint & HCINT_CHH) {
        oc->oc_done = 1;
        task_wakeup(&oc->oc_wait, 0);
      }
    }
  }
}


// Run one transaction stage on a channel and wait for it to halt.
// Returns number of bytes transferred or an error
static int
otgh_xfer(otgh_t *oh, int ch, uint32_t hcchar, int pid,
          void *buf, size_t len, int is_in, int64_t deadline)
{
  otgh_chan_t *oc = &oh->oh_chan[ch];
  const uint32_t mps = hcchar & 0x7ff;
  const uint32_t pktcnt = len ? (len + mps - 1) / mps : 1;
  // IN transfers must be programmed as a multiple of the packet size,
  // a short packet ends the transfer early
  const uint32_t xfrsiz = is_in ? pktcnt * mps : len;

  if(is_in)
    hcchar |= HCCHAR_EPDIR;

  int q = irq_forbid(IRQ_LEVEL_IO);

  oc->oc_hcint = 0;
  oc->oc_done = 0;

  oh_wr(oh, OTGH_HCINT(ch), 0x7ff);
  oh_wr(oh, OTGH_HCTSIZ(ch), (pid << 29) | (pktcnt << 19) | xfrsiz);
  oh_wr(oh, OTGH_HCDMA(ch), (uint32_t)buf);
  asm volatile ("dsb"); // Make sure the core sees what we wrote to buf
  oh_wr(oh, OTGH_HCCHAR(ch), hcchar | HCCHAR_CHENA);

  while(!oc->oc_done) {
    if(task_sleep_deadline(&oc->oc_wait, deadline)) {
      // Halt the channel. The core finishes the current transaction
      // first, so give it a while
      oh_wr(oh, OTGH_HCCHAR(ch),
            oh_rd(oh, OTGH_HCCHAR(ch)) | HCCHAR_CHENA | HCCHAR_CHDIS);
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
    const uint32_t remain = oh_rd(oh, OTGH_HCTSIZ(ch)) & 0x7ffff;
    return xfrsiz - remain;
  }

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
  int r = otgh_xfer(oh, 0, hcchar, PID_SETUP, setup_buf, 8, 0, deadline);
  if(r < 0)
    return r;

  if(len) {
    if(!data_in)
      memcpy(data_buf, data, len);
    r = otgh_xfer(oh, 0, hcchar, PID_DATA1, data_buf, len, data_in,
                  deadline);
    if(r < 0)
      return r;
    actual = MIN(r, len);
    if(data_in)
      memcpy(data, data_buf, actual);
  }

  // Status stage goes in the opposite direction of the data stage,
  // IN if there was no data stage
  r = otgh_xfer(oh, 0, hcchar, PID_DATA1, data_buf, 0, !(len && data_in),
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


static const usb_host_ctrl_vtable_t otgh_vtable = {
  .control = otgh_control,
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

  for(int i = 0; i < OTGH_NUM_CHANNELS; i++) {
    oh_wr(oh, OTGH_HCINTMSK(i), 0);
    oh_wr(oh, OTGH_HCINT(i), 0x7ff);
  }

  // In DMA mode channel halted is the only interrupt we need
  for(int i = 0; i < ARRAYSIZE(oh->oh_chan); i++)
    oh_wr(oh, OTGH_HCINTMSK(i), HCINT_CHH);
  oh_wr(oh, OTGH_HAINTMSK, (1 << ARRAYSIZE(oh->oh_chan)) - 1);

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


__attribute__((noreturn))
static void *
otgh_thread(void *arg)
{
  otgh_t *oh = arg;
  const char *name = oh->oh_uhc.uhc_dev.d_name;

  otgh_core_init(oh);
  irq_enable_fn_arg(oh->oh_irq, IRQ_LEVEL_IO, otgh_irq, oh);

  hprt_modify(oh, 0, HPRT_PPWR);
  if(oh->oh_vbus != GPIO_UNUSED) {
    gpio_conf_output(oh->oh_vbus, GPIO_PUSH_PULL, GPIO_SPEED_LOW,
                     GPIO_PULL_NONE);
    gpio_set_output(oh->oh_vbus, 1);
  }

  while(1) {
    if(!(oh_rd(oh, OTGH_HPRT) & HPRT_PCSTS)) {
      otgh_port_wait(oh, 0);
      continue;
    }

    // Let the connection settle (USB 2.0 spec 7.1.7.3)
    usleep(100000);
    if(!(oh_rd(oh, OTGH_HPRT) & HPRT_PCSTS))
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
          oh->oh_disconnects == disconnects) {
      otgh_port_wait(oh, 0);
    }

    usb_host_detach(&oh->oh_uhc);
  }
}


static void
otgh_print_info(struct device *d, struct stream *st)
{
  otgh_t *oh = (otgh_t *)d;
  static const char *speeds[4] = {"high", "full", "low", "?"};

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
}


static const device_class_t otgh_class = {
  .dc_class_name = "usb-host",
  .dc_print_info = otgh_print_info,
};


static void
stm32_otg_host_create(uint32_t base, int irq, gpio_t vbus, const char *name)
{
  otgh_t *oh = calloc(1, sizeof(otgh_t));
  oh->oh_base = base;
  oh->oh_vbus = vbus;
  oh->oh_uhc.uhc_vtable = &otgh_vtable;
  oh->oh_bounce = xalloc(OTGH_BOUNCE_SIZE, 64,
                         MEM_TYPE_DMA | MEM_TYPE_NO_CACHE);
  oh->oh_irq = irq;
  mutex_init(&oh->oh_ctrl_mutex, "usbhctrl");

  oh->oh_uhc.uhc_dev.d_name = name;
  oh->oh_uhc.uhc_dev.d_class = &otgh_class;
  device_register(&oh->oh_uhc.uhc_dev);

  // Core init sleeps (forcing host mode takes 25ms), so do it in the thread
  thread_create(otgh_thread, oh, 2048, name, 0, 4);
}
