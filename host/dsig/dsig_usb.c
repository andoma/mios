#include "dsig_usb.h"

#include <libusb.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define USB_CLASS_VENDOR 0xff
#define EP_SIZE 64

// A logical dsig-over-usb message can span multiple EP_SIZE packets
// (see usb_dsig.c's fragmentation on the guest side); libusb/the OS
// USB stack accumulates packets into one bulk-IN completion up to
// this buffer size or until a short packet ends the transfer, so this
// just needs to be >= the largest message we'll ever receive.
#define RX_BUF_SIZE 512

// Read/write timeout: also doubles as the poll interval for "should this
// thread stop" and "should we retry discovery", so keep it short.
#define IO_TIMEOUT_MS 300

struct dsig_usb {
  uint16_t vid, pid;
  uint8_t subclass;

  libusb_context *usb;
  dsig_t *bus;

  pthread_t rx_tid;
  int running;

  pthread_mutex_t lock; // guards h/iface/ep_*/connected
  libusb_device_handle *h;
  uint8_t iface, ep_out, ep_in;
  int connected;

  uint8_t cnt; // rolling frag counter, mirrors the guest's usb_dsig.c
};


static int
find_interface(libusb_context *usb, uint16_t vid, uint16_t pid,
              uint8_t subclass, libusb_device_handle **handle_out,
              uint8_t *iface_out, uint8_t *ep_out_out, uint8_t *ep_in_out)
{
  libusb_device **devlist;
  ssize_t cnt = libusb_get_device_list(usb, &devlist);
  int found = -1;

  for(ssize_t i = 0; i < cnt && found < 0; i++) {
    struct libusb_device_descriptor desc;
    if(libusb_get_device_descriptor(devlist[i], &desc) != 0)
      continue;
    if(desc.idVendor != vid)
      continue;
    if(pid && desc.idProduct != pid)
      continue;

    struct libusb_config_descriptor *cfg;
    if(libusb_get_active_config_descriptor(devlist[i], &cfg) != 0)
      continue;

    for(int j = 0; j < cfg->bNumInterfaces && found < 0; j++) {
      const struct libusb_interface *iface = &cfg->interface[j];
      for(int a = 0; a < iface->num_altsetting && found < 0; a++) {
        const struct libusb_interface_descriptor *alt = &iface->altsetting[a];
        if(alt->bInterfaceClass != USB_CLASS_VENDOR)
          continue;
        if(alt->bInterfaceSubClass != subclass)
          continue;

        uint8_t ep_out = 0, ep_in = 0;
        for(int e = 0; e < alt->bNumEndpoints; e++) {
          uint8_t addr = alt->endpoint[e].bEndpointAddress;
          if(addr & 0x80)
            ep_in = addr;
          else
            ep_out = addr;
        }

        if(ep_out && ep_in) {
          libusb_device_handle *hd;
          if(libusb_open(devlist[i], &hd) == 0) {
            *handle_out = hd;
            *iface_out = alt->bInterfaceNumber;
            *ep_out_out = ep_out;
            *ep_in_out = ep_in;
            found = 0;
          }
        }
      }
    }
    libusb_free_config_descriptor(cfg);
  }

  libusb_free_device_list(devlist, 1);
  return found;
}


static void
disconnect_locked(dsig_usb_t *t)
{
  if(t->connected) {
    libusb_release_interface(t->h, t->iface);
    libusb_close(t->h);
    t->connected = 0;
  }
}


static void *
rx_thread(void *arg)
{
  dsig_usb_t *t = arg;
  uint8_t buf[RX_BUF_SIZE];

  while(t->running) {
    pthread_mutex_lock(&t->lock);
    if(!t->connected) {
      libusb_device_handle *h;
      uint8_t iface, ep_out, ep_in;
      if(!find_interface(t->usb, t->vid, t->pid, t->subclass,
                         &h, &iface, &ep_out, &ep_in)) {
        libusb_detach_kernel_driver(h, iface);
        if(libusb_claim_interface(h, iface) == 0) {
          t->h = h;
          t->iface = iface;
          t->ep_out = ep_out;
          t->ep_in = ep_in;
          t->connected = 1;
        } else {
          libusb_close(h);
        }
      }
    }
    int connected = t->connected;
    libusb_device_handle *h = t->h;
    uint8_t ep_in = t->ep_in;
    pthread_mutex_unlock(&t->lock);

    if(!connected) {
      usleep(IO_TIMEOUT_MS * 1000);
      continue;
    }

    int len;
    int r = libusb_bulk_transfer(h, ep_in, buf, sizeof(buf), &len,
                                 IO_TIMEOUT_MS);
    if(r == LIBUSB_ERROR_TIMEOUT)
      continue;
    if(r != 0) {
      pthread_mutex_lock(&t->lock);
      disconnect_locked(t);
      pthread_mutex_unlock(&t->lock);
      continue;
    }

    if(len < 2)
      continue;
    if((buf[1] & 0xc0) != 0xc0)
      continue; // fragmented, not supported (guest never sends these)

    uint32_t id = buf[0] | ((buf[1] & 0xf) << 8);
    dsig_input(t->bus, id, buf + 2, len - 2);
  }
  return NULL;
}


dsig_usb_t *
dsig_usb_create(uint16_t vid, uint16_t pid, uint8_t subclass)
{
  dsig_usb_t *t = calloc(1, sizeof(*t));
  if(t == NULL)
    return NULL;

  if(libusb_init(&t->usb) != 0) {
    free(t);
    return NULL;
  }

  t->vid = vid;
  t->pid = pid;
  t->subclass = subclass;
  pthread_mutex_init(&t->lock, NULL);
  return t;
}


int
dsig_usb_start(dsig_usb_t *t, dsig_t *bus)
{
  t->bus = bus;
  t->running = 1;
  if(pthread_create(&t->rx_tid, NULL, rx_thread, t)) {
    t->running = 0;
    return -1;
  }
  return 0;
}


void
dsig_usb_tx(void *opaque, uint32_t signal, const void *data, size_t len)
{
  dsig_usb_t *t = opaque;

  if(signal > 0xfff || len > EP_SIZE - 2)
    return;

  uint8_t frame[EP_SIZE];
  frame[0] = signal;
  frame[1] = ((signal >> 8) & 0xf) | 0xc0 | ((t->cnt & 0x3) << 4);
  memcpy(frame + 2, data, len);

  pthread_mutex_lock(&t->lock);
  if(t->connected) {
    t->cnt++;
    int transferred;
    int r = libusb_bulk_transfer(t->h, t->ep_out, frame, len + 2,
                                 &transferred, IO_TIMEOUT_MS);
    // A transfer that's an exact multiple of the max packet size has
    // no natural short packet to mark its end -- the guest's usb_dsig.c
    // waits for one to know the logical transfer is complete, so send
    // an explicit ZLP terminator (len + 2 can only ever equal EP_SIZE
    // once, since len is capped at EP_SIZE - 2 above).
    if(r == 0 && len + 2 == EP_SIZE) {
      r = libusb_bulk_transfer(t->h, t->ep_out, NULL, 0,
                               &transferred, IO_TIMEOUT_MS);
    }
    if(r != 0)
      disconnect_locked(t);
  }
  pthread_mutex_unlock(&t->lock);
}


int
dsig_usb_connected(dsig_usb_t *t)
{
  pthread_mutex_lock(&t->lock);
  int c = t->connected;
  pthread_mutex_unlock(&t->lock);
  return c;
}


void
dsig_usb_destroy(dsig_usb_t *t)
{
  if(t == NULL)
    return;
  if(t->running) {
    t->running = 0;
    pthread_join(t->rx_tid, NULL);
  }
  pthread_mutex_lock(&t->lock);
  disconnect_locked(t);
  pthread_mutex_unlock(&t->lock);
  pthread_mutex_destroy(&t->lock);
  libusb_exit(t->usb);
  free(t);
}
