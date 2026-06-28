#include "mcp_transport.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <errno.h>
#include <glob.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <sys/file.h>
#include <time.h>
#include <zlib.h>
#include <libusb.h>

#define USB_CLASS_VENDOR 0xff
#define SERIAL_MAX_FRAME 256

#define MCP_HELLO        0x06
#define MCP_HELLO_MAGIC  "MIOS-MCP"

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

  // Exclusive access, matching sterm: TIOCEXCL blocks further opens at the
  // tty layer; flock is the advisory lock cooperating tools (sterm) honor.
  if(ioctl(fd, TIOCEXCL) || flock(fd, LOCK_EX | LOCK_NB)) {
    close(fd);
    return -1;
  }
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

// Try to pull one complete, CRC-valid frame out of an accumulation buffer.
// Returns payload length (copied into out) or -1 if none yet.
static int
hdlc_deframe(uint8_t *rx, size_t *rxlen, uint8_t *out, size_t cap)
{
  size_t start = 0;
  while(start < *rxlen && rx[start] != 0x7e)
    start++;
  if(start) {                       // drop leading garbage
    memmove(rx, rx + start, *rxlen - start);
    *rxlen -= start;
  }
  if(*rxlen < 2)
    return -1;

  size_t end = 1;                   // closing flag after the opening one
  while(end < *rxlen && rx[end] != 0x7e)
    end++;
  if(end >= *rxlen)
    return -1;                      // incomplete

  uint8_t frame[SERIAL_MAX_FRAME + 4];
  size_t flen = 0;
  int esc = 0;
  for(size_t i = 1; i < end; i++) {
    uint8_t b = rx[i];
    if(esc) {
      if(flen < sizeof(frame)) frame[flen++] = b ^ 0x20;
      esc = 0;
    } else if(b == 0x7d) {
      esc = 1;
    } else {
      if(flen < sizeof(frame)) frame[flen++] = b;
    }
  }

  size_t consumed = end + 1;
  memmove(rx, rx + consumed, *rxlen - consumed);
  *rxlen -= consumed;

  if(flen >= 5 && crc32(0, frame, flen) == 0xffffffff) {
    size_t plen = flen - 4;
    if(plen > cap)
      plen = cap;
    memcpy(out, frame, plen);
    return plen;
  }
  return -1;                        // bad/empty frame, caller reads more
}

static int
serial_deframe(mcp_xport_t *x, uint8_t *buf, size_t cap)
{
  return hdlc_deframe(x->rxbuf, &x->rxlen, buf, cap);
}


// Open a port and listen briefly for an MCP hello beacon. Returns 1 if the
// port speaks MCP. Ports held by another program (flock) are skipped.
static int
serial_probe(const char *path, int timeout_ms)
{
  int fd = serial_open(path);
  if(fd < 0)
    return 0;

  uint8_t rx[1024];
  size_t rxlen = 0;
  struct timespec t0;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  int found = 0;

  while(!found) {
    uint8_t msg[SERIAL_MAX_FRAME];
    int r = hdlc_deframe(rx, &rxlen, msg, sizeof(msg));
    if(r >= 1) {
      if(msg[0] == MCP_HELLO &&
         r - 1 >= (int)strlen(MCP_HELLO_MAGIC) &&
         !memcmp(msg + 1, MCP_HELLO_MAGIC, strlen(MCP_HELLO_MAGIC)))
        found = 1;
      continue;
    }

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long elapsed = (now.tv_sec - t0.tv_sec) * 1000 +
                   (now.tv_nsec - t0.tv_nsec) / 1000000;
    if(elapsed >= timeout_ms)
      break;

    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);
    struct timeval tv = { 0, 100000 };
    if(select(fd + 1, &rfds, NULL, NULL, &tv) > 0 && rxlen < sizeof(rx)) {
      ssize_t n = read(fd, rx + rxlen, sizeof(rx) - rxlen);
      if(n > 0)
        rxlen += n;
    }
  }

  close(fd);
  return found;
}

int
mcp_serial_scan(char (*paths)[256], int max)
{
  glob_t g;
  memset(&g, 0, sizeof(g));
  glob("/dev/ttyACM*", 0, NULL, &g);
  glob("/dev/ttyUSB*", GLOB_APPEND, NULL, &g);

  int n = 0;
  for(size_t i = 0; i < g.gl_pathc && n < max; i++) {
    // Beacon is ~1/s, so 1500 ms is enough to catch one.
    if(serial_probe(g.gl_pathv[i], 1500)) {
      snprintf(paths[n], 256, "%s", g.gl_pathv[i]);
      n++;
    }
  }
  globfree(&g);
  return n;
}

static char *
serial_resolve_auto(void)
{
  char paths[1][256];
  if(mcp_serial_scan(paths, 1) >= 1)
    return strdup(paths[0]);
  return NULL;
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

static int
xport_open_serial_path(mcp_xport_t *x, const char *path, const char **errstr)
{
  x->fd = serial_open(path);
  if(x->fd < 0) {
    *errstr = "Failed to open serial port (busy or missing)";
    return -1;
  }
  x->is_serial = 1;
  x->max_payload = SERIAL_MAX_FRAME - 1;
  return 0;
}

// USB is found by enumeration (vendor id), so it is stable and needs no
// beacon scan. Returns 0 on success.
static int
xport_open_usb(mcp_xport_t *x, mcp_context_t *ctx, uint8_t subclass)
{
  if(find_mcp_interface(ctx->usb, ctx->usb_vid, ctx->usb_pid, subclass,
                        &x->h, &x->iface, &x->ep_out, &x->ep_in))
    return -1;
  libusb_detach_kernel_driver(x->h, x->iface);
  if(libusb_claim_interface(x->h, x->iface)) {
    libusb_close(x->h);
    return -1;
  }
  x->max_payload = 63; // one 64-byte bulk packet, minus the type byte
  return 0;
}

// Resolve "auto"/fallback serial: scan for the hello beacon once and cache.
// Drops the cache if the cached port can no longer be opened.
static int
xport_open_serial_auto(mcp_xport_t *x, mcp_context_t *ctx, const char **errstr)
{
  if(!ctx->serial_resolved)
    ctx->serial_resolved = serial_resolve_auto();
  if(!ctx->serial_resolved)
    return -1;
  if(xport_open_serial_path(x, ctx->serial_resolved, errstr)) {
    free(ctx->serial_resolved);
    ctx->serial_resolved = NULL;
    return -1;
  }
  return 0;
}

mcp_xport_t *
mcp_xport_open(mcp_context_t *ctx, uint8_t subclass, const char **errstr)
{
  mcp_xport_t *x = calloc(1, sizeof(*x));

  // Explicit serial device path
  if(ctx->serial && ctx->serial[0] && strcmp(ctx->serial, "auto")) {
    if(xport_open_serial_path(x, ctx->serial, errstr)) {
      free(x);
      return NULL;
    }
    return x;
  }

  // Explicit "auto": serial only, detected via the beacon
  if(ctx->serial && !strcmp(ctx->serial, "auto")) {
    if(xport_open_serial_auto(x, ctx, errstr)) {
      if(!ctx->serial_resolved)
        *errstr = "No MCP serial port detected (no hello beacon found)";
      free(x);
      return NULL;
    }
    return x;
  }

  // Default: USB first (stable, enumerated by vendor id), then fall back to
  // serial auto-detect for devices without USB.
  if(xport_open_usb(x, ctx, subclass) == 0)
    return x;

  if(xport_open_serial_auto(x, ctx, errstr) == 0)
    return x;

  *errstr = "No MIOS device found (USB, or serial with an MCP beacon)";
  free(x);
  return NULL;
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
