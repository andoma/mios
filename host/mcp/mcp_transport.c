#include "mcp_transport.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <errno.h>
#include <sys/select.h>
#include <time.h>
#include <zlib.h>
#include <libusb.h>

#define USB_CLASS_VENDOR 0xff
#define SERIAL_MAX_FRAME 256

struct mcp_xport {
  int is_serial;

  // USB backend
  libusb_device_handle *h;
  uint8_t iface, ep_out, ep_in;

  // Serial backend
  int fd;
  uint8_t rxbuf[1024];
  size_t rxlen;

  size_t max_payload;
};


// ---------------- USB backend ----------------

static int
find_mcp_interface(libusb_context *usb, uint16_t vid, uint16_t pid,
                   uint8_t subclass, libusb_device_handle **handle_out,
                   uint8_t *iface_out, uint8_t *ep_out_out, uint8_t *ep_in_out)
{
  libusb_device **devlist;
  ssize_t cnt = libusb_get_device_list(usb, &devlist);

  for(ssize_t i = 0; i < cnt; i++) {
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

    int found = 0;
    for(int j = 0; j < cfg->bNumInterfaces && !found; j++) {
      const struct libusb_interface *iface = &cfg->interface[j];
      for(int a = 0; a < iface->num_altsetting && !found; a++) {
        const struct libusb_interface_descriptor *alt = &iface->altsetting[a];
        if(alt->bInterfaceClass != USB_CLASS_VENDOR)
          continue;
        if(alt->bInterfaceSubClass != subclass)
          continue;
        if(alt->bNumEndpoints != 2)
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
            found = 1;
          }
        }
      }
    }
    libusb_free_config_descriptor(cfg);

    if(found) {
      libusb_free_device_list(devlist, 1);
      return 0;
    }
  }

  libusb_free_device_list(devlist, 1);
  return -1;
}


// ---------------- Serial backend (HDLC + CRC32) ----------------

static int
serial_open(const char *path)
{
  int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
  if(fd < 0)
    return -1;

  struct termios t;
  if(tcgetattr(fd, &t)) {
    close(fd);
    return -1;
  }
  cfmakeraw(&t);
  t.c_cflag |= CLOCAL | CREAD;
  t.c_cc[VMIN] = 0;
  t.c_cc[VTIME] = 0;
  cfsetispeed(&t, B115200);
  cfsetospeed(&t, B115200);
  tcsetattr(fd, TCSANOW, &t);
  tcflush(fd, TCIOFLUSH);
  return fd;
}

static int
serial_send(int fd, const uint8_t *msg, size_t len)
{
  uint32_t crc = ~crc32(0, msg, len);
  uint8_t body[SERIAL_MAX_FRAME + 4];
  if(len > SERIAL_MAX_FRAME)
    return -1;
  memcpy(body, msg, len);
  memcpy(body + len, &crc, 4);  // little-endian host
  size_t blen = len + 4;

  uint8_t out[2 * (SERIAL_MAX_FRAME + 4) + 2];
  size_t o = 0;
  out[o++] = 0x7e;
  for(size_t i = 0; i < blen; i++) {
    if(body[i] == 0x7e || body[i] == 0x7d) {
      out[o++] = 0x7d;
      out[o++] = body[i] ^ 0x20;
    } else {
      out[o++] = body[i];
    }
  }
  out[o++] = 0x7e;

  size_t written = 0;
  while(written < o) {
    ssize_t n = write(fd, out + written, o - written);
    if(n < 0) {
      if(errno == EAGAIN)
        continue;
      return -1;
    }
    written += n;
  }
  return 0;
}

// Try to pull one complete, CRC-valid frame out of x->rxbuf.
// Returns payload length (copied into buf) or -1 if none yet.
static int
serial_deframe(mcp_xport_t *x, uint8_t *buf, size_t cap)
{
  size_t start = 0;
  while(start < x->rxlen && x->rxbuf[start] != 0x7e)
    start++;
  // drop leading garbage
  if(start) {
    memmove(x->rxbuf, x->rxbuf + start, x->rxlen - start);
    x->rxlen -= start;
  }
  if(x->rxlen < 2)
    return -1;

  // find closing flag (after the opening one at index 0)
  size_t end = 1;
  while(end < x->rxlen && x->rxbuf[end] != 0x7e)
    end++;
  if(end >= x->rxlen)
    return -1; // incomplete

  // unescape rxbuf[1..end)
  uint8_t frame[SERIAL_MAX_FRAME + 4];
  size_t flen = 0;
  int esc = 0;
  for(size_t i = 1; i < end; i++) {
    uint8_t b = x->rxbuf[i];
    if(esc) {
      if(flen < sizeof(frame)) frame[flen++] = b ^ 0x20;
      esc = 0;
    } else if(b == 0x7d) {
      esc = 1;
    } else {
      if(flen < sizeof(frame)) frame[flen++] = b;
    }
  }

  // consume up to and including the closing flag
  size_t consumed = end + 1;
  memmove(x->rxbuf, x->rxbuf + consumed, x->rxlen - consumed);
  x->rxlen -= consumed;

  if(flen >= 5 && crc32(0, frame, flen) == 0xffffffff) {
    size_t plen = flen - 4;
    if(plen > cap)
      plen = cap;
    memcpy(buf, frame, plen);
    return plen;
  }
  return -1; // bad/empty frame, caller will read more
}

static int
serial_recv(mcp_xport_t *x, uint8_t *buf, size_t cap, int timeout_ms)
{
  struct timespec t0;
  clock_gettime(CLOCK_MONOTONIC, &t0);

  while(1) {
    int r = serial_deframe(x, buf, cap);
    if(r >= 0)
      return r;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long elapsed = (now.tv_sec - t0.tv_sec) * 1000 +
                   (now.tv_nsec - t0.tv_nsec) / 1000000;
    long remain = timeout_ms - elapsed;
    if(remain <= 0)
      return 0;

    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(x->fd, &rfds);
    struct timeval tv = { remain / 1000, (remain % 1000) * 1000 };
    int s = select(x->fd + 1, &rfds, NULL, NULL, &tv);
    if(s <= 0)
      return 0;

    if(x->rxlen < sizeof(x->rxbuf)) {
      ssize_t n = read(x->fd, x->rxbuf + x->rxlen, sizeof(x->rxbuf) - x->rxlen);
      if(n > 0)
        x->rxlen += n;
    } else {
      x->rxlen = 0; // overflow, resync
    }
  }
}


// ---------------- Public API ----------------

mcp_xport_t *
mcp_xport_open(mcp_context_t *ctx, uint8_t subclass, const char **errstr)
{
  mcp_xport_t *x = calloc(1, sizeof(*x));

  if(ctx->serial && ctx->serial[0]) {
    x->is_serial = 1;
    x->fd = serial_open(ctx->serial);
    if(x->fd < 0) {
      *errstr = "Failed to open serial port";
      free(x);
      return NULL;
    }
    x->max_payload = SERIAL_MAX_FRAME - 1;
    return x;
  }

  if(find_mcp_interface(ctx->usb, ctx->usb_vid, ctx->usb_pid, subclass,
                        &x->h, &x->iface, &x->ep_out, &x->ep_in)) {
    *errstr = "No MIOS device with MCP interface found";
    free(x);
    return NULL;
  }

  libusb_detach_kernel_driver(x->h, x->iface);
  if(libusb_claim_interface(x->h, x->iface)) {
    libusb_close(x->h);
    *errstr = "Failed to claim MCP USB interface";
    free(x);
    return NULL;
  }
  x->max_payload = 63; // one 64-byte bulk packet, minus the type byte
  return x;
}

size_t
mcp_xport_max_payload(mcp_xport_t *x)
{
  return x->max_payload;
}

int
mcp_xport_send(mcp_xport_t *x, const uint8_t *msg, size_t len)
{
  if(x->is_serial)
    return serial_send(x->fd, msg, len);

  int transferred;
  return libusb_bulk_transfer(x->h, x->ep_out, (uint8_t *)msg, len,
                              &transferred, 5000) ? -1 : 0;
}

int
mcp_xport_recv(mcp_xport_t *x, uint8_t *buf, size_t cap, int timeout_ms)
{
  if(x->is_serial)
    return serial_recv(x, buf, cap, timeout_ms);

  int len;
  int r = libusb_bulk_transfer(x->h, x->ep_in, buf, cap, &len, timeout_ms);
  if(r == LIBUSB_ERROR_TIMEOUT)
    return 0;
  if(r != 0)
    return -1;
  return len;
}

void
mcp_xport_close(mcp_xport_t *x)
{
  if(x->is_serial) {
    if(x->fd >= 0)
      close(x->fd);
  } else {
    libusb_release_interface(x->h, x->iface);
    libusb_close(x->h);
  }
  free(x);
}
