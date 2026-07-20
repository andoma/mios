#include "jlink.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libusb.h>

#define JLINK_USB_VID 0x1366

#define EMU_CMD_VERSION           0x01
#define EMU_CMD_SET_SPEED         0x05
#define EMU_CMD_SELECT_IF         0xc7
#define EMU_CMD_GET_MAX_MEM_BLOCK 0xd4
#define EMU_CMD_GET_CAPS          0xe8
#define EMU_CMD_HW_JTAG3          0xcf

#define EMU_CAP_GET_MAX_BLOCK_SIZE (1u << 11)
#define EMU_CAP_SELECT_IF          (1u << 17)

#define JLINK_IF_SWD 1

#define USB_TIMEOUT_MS 2000

struct jlink {
  libusb_context *ctx;
  libusb_device_handle *h;
  int iface;
  uint8_t ep_in;
  uint8_t ep_out;
  uint32_t caps;
  size_t max_mem_block;
  char serial[64];
  char version[128];
  char errmsg[256];
  uint8_t *iobuf;
};

const char *
jlink_errmsg(jlink_t *jl)
{
  return jl->errmsg;
}

const char *
jlink_version(jlink_t *jl)
{
  return jl->version;
}

const char *
jlink_serial(jlink_t *jl)
{
  return jl->serial;
}

static int
jl_write(jlink_t *jl, const void *buf, size_t len)
{
  int done;
  int r = libusb_bulk_transfer(jl->h, jl->ep_out, (void *)buf, len,
                               &done, USB_TIMEOUT_MS);
  if(r < 0 || done != (int)len) {
    snprintf(jl->errmsg, sizeof(jl->errmsg), "USB write failed: %s",
             libusb_strerror(r));
    return -1;
  }
  return 0;
}

static int
jl_read(jlink_t *jl, void *buf, size_t len)
{
  uint8_t *p = buf;
  while(len) {
    int done;
    int r = libusb_bulk_transfer(jl->h, jl->ep_in, p, len,
                                 &done, USB_TIMEOUT_MS);
    if(r < 0) {
      snprintf(jl->errmsg, sizeof(jl->errmsg), "USB read failed: %s",
               libusb_strerror(r));
      return -1;
    }
    p += done;
    len -= done;
  }
  return 0;
}

static int
jl_read_u32(jlink_t *jl, uint32_t *v)
{
  uint8_t buf[4];
  if(jl_read(jl, buf, 4))
    return -1;
  *v = buf[0] | (buf[1] << 8) | (buf[2] << 16) | ((uint32_t)buf[3] << 24);
  return 0;
}

// Locate the vendor-specific interface (class/subclass/protocol all 0xff)
// with two bulk endpoints
static int
jl_find_interface(jlink_t *jl, libusb_device *dev)
{
  struct libusb_config_descriptor *cfg;
  if(libusb_get_active_config_descriptor(dev, &cfg) < 0)
    return -1;

  int r = -1;
  for(int i = 0; i < cfg->bNumInterfaces && r; i++) {
    const struct libusb_interface_descriptor *id =
      &cfg->interface[i].altsetting[0];

    if(id->bInterfaceClass != 0xff || id->bInterfaceSubClass != 0xff ||
       id->bInterfaceProtocol != 0xff || id->bNumEndpoints != 2)
      continue;

    for(int e = 0; e < 2; e++) {
      const struct libusb_endpoint_descriptor *ep = &id->endpoint[e];
      if((ep->bmAttributes & 3) != LIBUSB_TRANSFER_TYPE_BULK)
        goto next;
      if(ep->bEndpointAddress & LIBUSB_ENDPOINT_IN)
        jl->ep_in = ep->bEndpointAddress;
      else
        jl->ep_out = ep->bEndpointAddress;
    }
    if(jl->ep_in && jl->ep_out) {
      jl->iface = id->bInterfaceNumber;
      r = 0;
    }
  next:;
  }
  libusb_free_config_descriptor(cfg);
  return r;
}

static int
jl_init(jlink_t *jl)
{
  // Firmware version string
  uint8_t cmd = EMU_CMD_VERSION;
  uint8_t lenbuf[2];
  if(jl_write(jl, &cmd, 1) || jl_read(jl, lenbuf, 2))
    return -1;
  size_t vlen = lenbuf[0] | (lenbuf[1] << 8);
  char *vbuf = alloca(vlen + 1);
  if(jl_read(jl, vbuf, vlen))
    return -1;
  vbuf[vlen] = 0;
  // Trim trailing newlines/nuls
  for(char *p = vbuf; *p; p++)
    if(*p == '\r' || *p == '\n')
      *p = ' ';
  snprintf(jl->version, sizeof(jl->version), "%s", vbuf);

  cmd = EMU_CMD_GET_CAPS;
  if(jl_write(jl, &cmd, 1) || jl_read_u32(jl, &jl->caps))
    return -1;

  if(!(jl->caps & EMU_CAP_SELECT_IF)) {
    snprintf(jl->errmsg, sizeof(jl->errmsg),
             "Probe does not support interface selection (no SWD)");
    return -1;
  }

  jl->max_mem_block = 2048;
  if(jl->caps & EMU_CAP_GET_MAX_BLOCK_SIZE) {
    cmd = EMU_CMD_GET_MAX_MEM_BLOCK;
    uint32_t v;
    if(jl_write(jl, &cmd, 1) || jl_read_u32(jl, &v))
      return -1;
    if(v >= 256 && v <= 65536)
      jl->max_mem_block = v;
  }

  jl->iobuf = malloc(jl->max_mem_block);
  return 0;
}

jlink_t *
jlink_open(const char *serial)
{
  jlink_t *jl = calloc(1, sizeof(jlink_t));

  if(libusb_init(&jl->ctx) < 0) {
    free(jl);
    return NULL;
  }

  libusb_device **list;
  ssize_t num = libusb_get_device_list(jl->ctx, &list);
  int found = 0;

  snprintf(jl->errmsg, sizeof(jl->errmsg), "No J-Link probe found");

  for(ssize_t i = 0; i < num && !found; i++) {
    struct libusb_device_descriptor desc;
    if(libusb_get_device_descriptor(list[i], &desc) < 0)
      continue;
    if(desc.idVendor != JLINK_USB_VID)
      continue;

    libusb_device_handle *h;
    if(libusb_open(list[i], &h) < 0) {
      snprintf(jl->errmsg, sizeof(jl->errmsg),
               "Found J-Link but failed to open it (permissions?)");
      continue;
    }

    if(desc.iSerialNumber) {
      libusb_get_string_descriptor_ascii(h, desc.iSerialNumber,
                                         (uint8_t *)jl->serial,
                                         sizeof(jl->serial));
    }

    if(serial != NULL && strcmp(serial, jl->serial)) {
      snprintf(jl->errmsg, sizeof(jl->errmsg),
               "No J-Link probe with serial %s found", serial);
      libusb_close(h);
      continue;
    }

    jl->h = h;
    if(jl_find_interface(jl, list[i])) {
      snprintf(jl->errmsg, sizeof(jl->errmsg),
               "No vendor interface on J-Link %s", jl->serial);
      libusb_close(h);
      jl->h = NULL;
      continue;
    }
    found = 1;
  }
  libusb_free_device_list(list, 1);

  if(!found)
    goto fail;

  libusb_detach_kernel_driver(jl->h, jl->iface); // Linux; noop elsewhere

  if(libusb_claim_interface(jl->h, jl->iface) < 0) {
    snprintf(jl->errmsg, sizeof(jl->errmsg),
             "Failed to claim USB interface (probe in use?)");
    goto fail;
  }

  if(jl_init(jl))
    goto fail;

  return jl;

fail:
  fprintf(stderr, "%s\n", jl->errmsg);
  jlink_close(jl);
  return NULL;
}

void
jlink_close(jlink_t *jl)
{
  if(jl == NULL)
    return;
  if(jl->h != NULL) {
    libusb_release_interface(jl->h, jl->iface);
    libusb_close(jl->h);
  }
  libusb_exit(jl->ctx);
  free(jl->iobuf);
  free(jl);
}

int
jlink_target_voltage(jlink_t *jl)
{
  uint8_t cmd = 0x07; // EMU_CMD_GET_STATE
  uint8_t buf[8];
  if(jl_write(jl, &cmd, 1) || jl_read(jl, buf, 8))
    return -1;
  return buf[0] | (buf[1] << 8);
}

int
jlink_select_swd(jlink_t *jl)
{
  uint8_t cmd[2] = {EMU_CMD_SELECT_IF, JLINK_IF_SWD};
  uint32_t prev;
  if(jl_write(jl, cmd, 2) || jl_read_u32(jl, &prev))
    return -1;
  return 0;
}

int
jlink_set_speed_khz(jlink_t *jl, unsigned int khz)
{
  uint8_t cmd[3] = {EMU_CMD_SET_SPEED, khz & 0xff, (khz >> 8) & 0xff};
  return jl_write(jl, cmd, 3);
}

size_t
jlink_swd_max_bits(const jlink_t *jl)
{
  // Command is 4 + 2 * nbytes, response is nbytes + 1; both must fit
  // in the probe's buffer
  return ((jl->max_mem_block - 4) / 2 - 1) * 8;
}

int
jlink_swd_io(jlink_t *jl, const uint8_t *dir, const uint8_t *out,
             uint8_t *in, size_t nbits)
{
  const size_t nbytes = (nbits + 7) / 8;
  uint8_t *cmd = jl->iobuf;

  if(nbits == 0)
    return 0;

  if(nbits > jlink_swd_max_bits(jl)) {
    snprintf(jl->errmsg, sizeof(jl->errmsg), "SWD transfer too large");
    return -1;
  }

  cmd[0] = EMU_CMD_HW_JTAG3;
  cmd[1] = 0;
  cmd[2] = nbits & 0xff;
  cmd[3] = (nbits >> 8) & 0xff;
  memcpy(cmd + 4, dir, nbytes);
  memcpy(cmd + 4 + nbytes, out, nbytes);

  if(jl_write(jl, cmd, 4 + 2 * nbytes))
    return -1;

  uint8_t *rsp = jl->iobuf;
  if(jl_read(jl, rsp, nbytes + 1))
    return -1;

  if(rsp[nbytes] != 0) {
    snprintf(jl->errmsg, sizeof(jl->errmsg),
             "Probe returned I/O error %d", rsp[nbytes]);
    return -1;
  }
  if(in != NULL)
    memcpy(in, rsp, nbytes);
  return 0;
}
