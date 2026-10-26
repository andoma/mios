// Serial port class drivers for the USB host stack: CDC-ACM, and vendor
// specific interfaces with a pair of bulk endpoints (such as the AT
// ports of cellular modems). Each port is exposed as a stream, see
// usb_host_serial_open()

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <sys/param.h>

#include <mios/stream.h>
#include <mios/task.h>
#include <mios/error.h>

#include <usb/usb_host.h>

#include "irq.h"

#define SERIAL_MAX_UNITS     8
#define SERIAL_BUF_SIZE      512  // DMA buffers, multiple of the HS bulk MPS
#define SERIAL_RX_FIFO_SIZE  1024 // Must be a power of two

#define CDC_SET_LINE_CODING        0x20
#define CDC_SET_CONTROL_LINE_STATE 0x22

#define CDC_SUBTYPE_UNION 0x06

typedef struct usb_host_serial {
  stream_t s;

  usb_host_device_t *ud;  // NULL once detached
  usb_host_pipe_t *in;
  usb_host_pipe_t *out;

  usb_host_xfer_t rx_xfer;
  usb_host_xfer_t tx_xfer;
  uint8_t *rx_dma;
  uint8_t *tx_dma;

  mutex_t tx_mutex;
  task_waitable_t rx_wait;
  task_waitable_t tx_wait;

  // Below are protected by IRQ_LEVEL_IO
  uint16_t rx_rdptr;      // Free running fifo pointers
  uint16_t rx_wrptr;
  uint8_t rx_running;     // rx_xfer is submitted
  uint8_t tx_running;     // tx_xfer is submitted
  uint8_t gone;           // Device detached

  int refcount;           // Protected by serial_mutex

  uint8_t rx_fifo[SERIAL_RX_FIFO_SIZE];
} usb_host_serial_t;


static mutex_t serial_mutex = MUTEX_INITIALIZER("serial");
static usb_host_serial_t *serial_units[SERIAL_MAX_UNITS];


// Called with serial_mutex held
static void
serial_release(usb_host_serial_t *a)
{
  if(--a->refcount)
    return;
  free(a->rx_dma);
  free(a->tx_dma);
  free(a);
}


// Submit the RX transfer unless it's already running or the fifo can't
// take a full buffer. Called with IRQ_LEVEL_IO blocked
static void
serial_rx_start(usb_host_serial_t *a)
{
  if(a->rx_running || a->gone)
    return;
  const uint16_t used = a->rx_wrptr - a->rx_rdptr;
  if(SERIAL_RX_FIFO_SIZE - used < SERIAL_BUF_SIZE)
    return; // The reader restarts us once it has made room

  a->rx_xfer.ux_buf = a->rx_dma;
  a->rx_xfer.ux_len = SERIAL_BUF_SIZE;
  if(!usb_host_submit(a->ud, a->in, &a->rx_xfer))
    a->rx_running = 1;
}


static void
serial_rx_done(usb_host_xfer_t *ux)
{
  usb_host_serial_t *a = ux->ux_opaque;
  a->rx_running = 0;

  if(ux->ux_result > 0) {
    // serial_rx_start() made sure there is room for all of it
    for(int i = 0; i < ux->ux_result; i++)
      a->rx_fifo[a->rx_wrptr++ & (SERIAL_RX_FIFO_SIZE - 1)] = a->rx_dma[i];
    serial_rx_start(a);
  } else if(ux->ux_result == 0) {
    serial_rx_start(a);
  }
  // On errors the reader retries when it wakes up
  task_wakeup(&a->rx_wait, 1);
}


static void
serial_tx_done(usb_host_xfer_t *ux)
{
  usb_host_serial_t *a = ux->ux_opaque;
  a->tx_running = 0;
  task_wakeup(&a->tx_wait, 1);
}


static ssize_t
serial_read(stream_t *s, void *buf, size_t size, size_t required)
{
  usb_host_serial_t *a = (usb_host_serial_t *)s;
  uint8_t *d = buf;
  size_t i = 0;

  int q = irq_forbid(IRQ_LEVEL_IO);

  while(i < size) {
    if(a->rx_rdptr == a->rx_wrptr) {
      serial_rx_start(a);
      if(a->gone && i == 0) {
        irq_permit(q);
        return ERR_NOT_CONNECTED;
      }
      if(i >= required || a->gone)
        break;
      task_sleep(&a->rx_wait);
      continue;
    }
    d[i++] = a->rx_fifo[a->rx_rdptr++ & (SERIAL_RX_FIFO_SIZE - 1)];
  }
  serial_rx_start(a); // We may have made room
  irq_permit(q);
  return i;
}


static ssize_t
serial_write(stream_t *s, const void *buf, size_t size, int flags)
{
  usb_host_serial_t *a = (usb_host_serial_t *)s;
  const uint8_t *b = buf;
  size_t written = 0;
  error_t err = 0;

  mutex_lock(&a->tx_mutex);

  while(written < size) {
    const size_t chunk = MIN(size - written, SERIAL_BUF_SIZE);
    memcpy(a->tx_dma, b + written, chunk);
    a->tx_xfer.ux_buf = a->tx_dma;
    a->tx_xfer.ux_len = chunk;

    int q = irq_forbid(IRQ_LEVEL_IO);
    err = a->gone ? ERR_NOT_CONNECTED :
      usb_host_submit(a->ud, a->out, &a->tx_xfer);
    if(!err) {
      a->tx_running = 1;
      while(a->tx_running)
        task_sleep(&a->tx_wait);
      if(a->tx_xfer.ux_result < 0)
        err = a->tx_xfer.ux_result;
    }
    irq_permit(q);

    if(err)
      break;
    written += chunk;
  }

  mutex_unlock(&a->tx_mutex);
  return written ? written : err;
}


static task_waitable_t *
serial_poll(stream_t *s, poll_type_t type)
{
  usb_host_serial_t *a = (usb_host_serial_t *)s;

  irq_forbid(IRQ_LEVEL_IO);

  if(type == POLL_STREAM_WRITE)
    return NULL; // Writes block until the transfer is done

  if(a->rx_rdptr != a->rx_wrptr || a->gone)
    return NULL;
  serial_rx_start(a);
  return &a->rx_wait;
}


static void
serial_close(stream_t *s)
{
  usb_host_serial_t *a = (usb_host_serial_t *)s;
  mutex_lock(&serial_mutex);
  serial_release(a);
  mutex_unlock(&serial_mutex);
}


static const stream_vtable_t serial_vtable = {
  .read = serial_read,
  .write = serial_write,
  .poll = serial_poll,
  .close = serial_close,
};


stream_t *
usb_host_serial_open(int unit)
{
  if(unit < 0 || unit >= SERIAL_MAX_UNITS)
    return NULL;

  mutex_lock(&serial_mutex);
  usb_host_serial_t *a = serial_units[unit];
  if(a != NULL)
    a->refcount++;
  mutex_unlock(&serial_mutex);
  return a ? &a->s : NULL;
}


static void *
serial_attach(usb_host_device_t *ud, int ifnum,
              const struct usb_endpoint_descriptor *in_ed,
              const struct usb_endpoint_descriptor *out_ed)
{
  usb_host_serial_t *a = calloc(1, sizeof(usb_host_serial_t));
  a->s.vtable = &serial_vtable;
  a->ud = ud;
  a->refcount = 1;
  mutex_init(&a->tx_mutex, "sertx");
  // The host controller driver does cache maintenance
  a->rx_dma = xalloc(SERIAL_BUF_SIZE, 64, MEM_TYPE_DMA);
  a->tx_dma = xalloc(SERIAL_BUF_SIZE, 64, MEM_TYPE_DMA);
  a->rx_xfer.ux_done = serial_rx_done;
  a->rx_xfer.ux_opaque = a;
  a->tx_xfer.ux_done = serial_tx_done;
  a->tx_xfer.ux_opaque = a;

  a->in = usb_host_pipe_open(ud, in_ed);
  a->out = usb_host_pipe_open(ud, out_ed);

  int unit = -1;
  if(a->in != NULL && a->out != NULL) {
    mutex_lock(&serial_mutex);
    for(int i = 0; i < SERIAL_MAX_UNITS; i++) {
      if(serial_units[i] == NULL) {
        serial_units[i] = a;
        unit = i;
        break;
      }
    }
    mutex_unlock(&serial_mutex);
  }

  if(unit == -1) {
    if(a->in)
      usb_host_pipe_close(ud, a->in);
    if(a->out)
      usb_host_pipe_close(ud, a->out);
    mutex_lock(&serial_mutex);
    serial_release(a);
    mutex_unlock(&serial_mutex);
    return NULL;
  }

  // Assert DTR and RTS, many devices don't send anything before DTR.
  // Vendor serial ports of cellular modems take the same request
  usb_host_control(ud, 0x21, CDC_SET_CONTROL_LINE_STATE, 3, ifnum, NULL, 0);

  printf("%s: Serial port %d (interface %d)\n",
         ud->ud_dev.d_name, unit, ifnum);

  int q = irq_forbid(IRQ_LEVEL_IO);
  serial_rx_start(a);
  irq_permit(q);
  return a;
}


static void *
acm_probe(usb_host_device_t *ud, const struct usb_interface_descriptor *id)
{
  if(id->bInterfaceClass != 0x02 || id->bInterfaceSubClass != 0x02)
    return NULL;

  // The union functional descriptor names the data interface
  int data_ifnum = id->bInterfaceNumber + 1;
  const uint8_t *cs = (const void *)id;
  while((cs = usb_host_find_desc(ud, cs, USB_DESC_TYPE_CS_INTERFACE))) {
    if(cs[0] >= 5 && cs[2] == CDC_SUBTYPE_UNION) {
      data_ifnum = cs[4];
      break;
    }
  }

  const struct usb_interface_descriptor *did =
    usb_host_find_interface(ud, data_ifnum);
  if(did == NULL || did->bInterfaceClass != 0x0a || data_ifnum >= 32)
    return NULL;

  const struct usb_endpoint_descriptor *in_ed =
    usb_host_find_endpoint(ud, did, USB_ENDPOINT_BULK, 0x80);
  const struct usb_endpoint_descriptor *out_ed =
    usb_host_find_endpoint(ud, did, USB_ENDPOINT_BULK, 0);
  if(in_ed == NULL || out_ed == NULL)
    return NULL;

  // 115200 8N1. Meaningless for most devices, but some want it
  static const uint8_t line_coding[7] = {0x00, 0xc2, 0x01, 0x00, 0, 0, 8};
  uint8_t lc[7];
  memcpy(lc, line_coding, sizeof(lc));
  usb_host_control(ud, 0x21, CDC_SET_LINE_CODING, 0, id->bInterfaceNumber,
                   lc, sizeof(lc));

  usb_host_serial_t *a = serial_attach(ud, id->bInterfaceNumber, in_ed, out_ed);
  if(a != NULL)
    ud->ud_claimed |= 1 << data_ifnum;
  return a;
}


static void
serial_detach(usb_host_device_t *ud, void *instance)
{
  usb_host_serial_t *a = instance;

  int q = irq_forbid(IRQ_LEVEL_IO);
  a->gone = 1;
  task_wakeup(&a->rx_wait, 1);
  irq_permit(q);

  // Pending transfers complete (with an error) before these return
  usb_host_pipe_close(ud, a->in);
  usb_host_pipe_close(ud, a->out);
  a->ud = NULL;

  mutex_lock(&serial_mutex);
  for(int i = 0; i < SERIAL_MAX_UNITS; i++) {
    if(serial_units[i] == a)
      serial_units[i] = NULL;
  }
  serial_release(a);
  mutex_unlock(&serial_mutex);
}


static const usb_host_serial_id_t *vendor_ids;
static size_t vendor_num_ids;

static void *
vendor_probe(usb_host_device_t *ud, const struct usb_interface_descriptor *id)
{
  if(id->bInterfaceClass != 0xff)
    return NULL;

  for(size_t i = 0; i < vendor_num_ids; i++) {
    const usb_host_serial_id_t *usi = &vendor_ids[i];
    if(usi->vid != ud->ud_desc.idVendor ||
       usi->pid != ud->ud_desc.idProduct ||
       !(usi->interfaces & (1 << id->bInterfaceNumber)))
      continue;

    const struct usb_endpoint_descriptor *in_ed =
      usb_host_find_endpoint(ud, id, USB_ENDPOINT_BULK, 0x80);
    const struct usb_endpoint_descriptor *out_ed =
      usb_host_find_endpoint(ud, id, USB_ENDPOINT_BULK, 0);
    if(in_ed == NULL || out_ed == NULL)
      return NULL;
    return serial_attach(ud, id->bInterfaceNumber, in_ed, out_ed);
  }
  return NULL;
}


static usb_host_driver_t vendor_driver = {
  .uhd_probe = vendor_probe,
  .uhd_detach = serial_detach,
};


void
usb_host_serial_register(const usb_host_serial_id_t *ids, size_t num_ids)
{
  vendor_ids = ids;
  vendor_num_ids = num_ids;
  usb_host_driver_register(&vendor_driver);
}


static usb_host_driver_t acm_driver = {
  .uhd_probe = acm_probe,
  .uhd_detach = serial_detach,
};


void
usb_host_acm_register(void)
{
  usb_host_driver_register(&acm_driver);
}
