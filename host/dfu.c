#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/param.h>

#include <libusb.h>

#include "mios_image.h"

struct dfu_status {
  uint8_t bStatus;          // status code
  uint8_t bwPollTimeout[3]; // little endian 24-bit
  uint8_t bState;           // state enum
  uint8_t iString;          // not used
};

#define CTRL_OUT (DFU_HOST_TO_DEVICE | DFU_TYPE_CLASS | DFU_RECIP_INTERFACE)
#define CTRL_IN  (DFU_DEVICE_TO_HOST | DFU_TYPE_CLASS | DFU_RECIP_INTERFACE)

#define DFU_DETACH     0x00
#define DFU_DNLOAD     0x01
#define DFU_UPLOAD     0x02
#define DFU_GETSTATUS  0x03
#define DFU_CLRSTATUS  0x04
#define DFU_GETSTATE   0x05
#define DFU_ABORT      0x06

// bmRequestType values per DFU 1.1
#define DFU_TYPE_CLASS         (0x01 << 5)
#define DFU_RECIP_INTERFACE    0x01
#define DFU_HOST_TO_DEVICE     0x00
#define DFU_DEVICE_TO_HOST     0x80

// DFU status codes (subset)
#define DFU_STATUS_OK          0x00
#define DFU_STATUS_errTARGET   0x01
#define DFU_STATUS_errFILE     0x02
#define DFU_STATUS_errWRITE    0x03
#define DFU_STATUS_errERASE    0x04
#define DFU_STATUS_errCHECK_ERASED 0x05
#define DFU_STATUS_errADDRESS  0x07
#define DFU_STATUS_errUNKNOWN  0x0A

// DFU states (subset)
#define DFU_STATE_appIDLE            0
#define DFU_STATE_appDETACH          1
#define DFU_STATE_dfuIDLE            2
#define DFU_STATE_dfuDNLOAD_SYNC     3
#define DFU_STATE_dfuDNBUSY          4
#define DFU_STATE_dfuDNLOAD_IDLE     5
#define DFU_STATE_dfuMANIFEST_SYNC   6
#define DFU_STATE_dfuMANIFEST        7
#define DFU_STATE_dfuMANIFEST_WAIT_RESET 8
#define DFU_STATE_dfuUPLOAD_IDLE     9
#define DFU_STATE_dfuERROR           10

// ST bootloader command bytes (sent in DNLOAD block when wValue==0)
#define BL_CMD_GET                  0x00
#define BL_CMD_SET_ADDRESS_POINTER  0x21
#define BL_CMD_ERASE                0x41
#define BL_CMD_READ_UNPROTECT       0x92

#define INTERFACE 0

#define DFU_FUNC_DESC    0x21   // DFU Functional Descriptor type
#define DFU_FUNC_LEN_MIN 9

struct dfu_functional_descriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bmAttributes;
    uint16_t wDetachTimeOut;
    uint16_t wTransferSize;
    uint16_t bcdDFU;
} __attribute__((packed));

static const char *
dfu_state_str(uint8_t s)
{
  switch(s){
  case DFU_STATE_appIDLE: return "appIDLE";
  case DFU_STATE_appDETACH: return "appDETACH";
  case DFU_STATE_dfuIDLE: return "dfuIDLE";
  case DFU_STATE_dfuDNLOAD_SYNC: return "dfuDNLOAD-SYNC";
  case DFU_STATE_dfuDNBUSY: return "dfuDNBUSY";
  case DFU_STATE_dfuDNLOAD_IDLE: return "dfuDNLOAD-IDLE";
  case DFU_STATE_dfuMANIFEST_SYNC: return "dfuMANIFEST-SYNC";
  case DFU_STATE_dfuMANIFEST: return "dfuMANIFEST";
  case DFU_STATE_dfuMANIFEST_WAIT_RESET: return "dfuMANIFEST-WAIT-RESET";
  case DFU_STATE_dfuUPLOAD_IDLE: return "dfuUPLOAD-IDLE";
  case DFU_STATE_dfuERROR: return "dfuERROR";
  default: return "?";
  }
}

static const char *
dfu_status_str(uint8_t s)
{
  switch(s){
  case DFU_STATUS_OK: return "OK";
  case DFU_STATUS_errTARGET: return "errTARGET";
  case DFU_STATUS_errFILE: return "errFILE";
  case DFU_STATUS_errWRITE: return "errWRITE";
  case DFU_STATUS_errERASE: return "errERASE";
  case DFU_STATUS_errCHECK_ERASED: return "errCHECK_ERASED";
  case DFU_STATUS_errADDRESS: return "errADDRESS";
  case DFU_STATUS_errUNKNOWN: return "errUNKNOWN";
  default: return "status?";
  }
}

static void __attribute__((unused))
hexdump(const char *pfx, const void *data_, int len)
{
  int i, j, k;
  const uint8_t *data = data_;
  char buf[100];

  for(i = 0; i < len; i+= 16) {
    int p = snprintf(buf, sizeof(buf), "0x%06x: ", i);

    for(j = 0; j + i < len && j < 16; j++) {
      p += snprintf(buf + p, sizeof(buf) - p, "%s%02x ",
                    j==8 ? " " : "", data[i+j]);
    }
    const int cnt = (17 - j) * 3 + (j < 8);
    for(k = 0; k < cnt; k++)
      buf[p + k] = ' ';
    p += cnt;

    for(j = 0; j + i < len && j < 16; j++)
      buf[p++] = data[i+j] < 32 || data[i+j] > 126 ? '.' : data[i+j];
    buf[p] = 0;
    printf("%s: %s\n", pfx, buf);
  }
}



static int
ctrl_out(libusb_device_handle *h, uint8_t req, uint16_t wValue, const void* buf, uint16_t len)
{
  int r = libusb_control_transfer(h, CTRL_OUT, req, wValue, INTERFACE, (void *)buf, len, 5000);
  if(r != len)
    return -1;
  return 0;
}

static int
ctrl_in(libusb_device_handle *h, uint8_t req, uint16_t wValue, void* buf, uint16_t len)
{
  int r = libusb_control_transfer(h, CTRL_IN, req, wValue, INTERFACE, buf, len, 15000);
  if(r != len)
    return -1;
  return 0;
}

static uint32_t
get_poll_timeout(const struct dfu_status* st)
{
  return st->bwPollTimeout[0] | (st->bwPollTimeout[1] << 8) | (st->bwPollTimeout[2] << 16);
}

static int
dfu_getstatus(libusb_device_handle *h, struct dfu_status *st)
{
  return ctrl_in(h, DFU_GETSTATUS, 0, st, sizeof(*st));
}

static int
dfu_clrstatus(libusb_device_handle *h)
{
  return ctrl_out(h, DFU_CLRSTATUS, 0, NULL, 0);
}


__attribute__((unused))
static int
dfu_getstate(libusb_device_handle *h, uint8_t *st)
{
  return ctrl_in(h, DFU_GETSTATE, 0, st, 1);
}

static int
dfu_wait_while_busy(libusb_device_handle *h)
{
  struct dfu_status st;

  while(1) {
    int r = dfu_getstatus(h, &st);
    if(r < 0)
      return r;
    uint32_t wait = get_poll_timeout(&st);
    usleep(wait * 1000);

    if(st.bState == DFU_STATE_dfuDNBUSY)
      continue;
    if(st.bStatus != DFU_STATUS_OK) {
      printf("DFU error: %s (state=%s)\n", dfu_status_str(st.bStatus), dfu_state_str(st.bState));
      return -1;
    }
    return 0;
  }
}

static int
set_address_pointer(libusb_device_handle *h, uint32_t addr)
{
  uint8_t buf[5];
  buf[0] = BL_CMD_SET_ADDRESS_POINTER;
  buf[1] = (uint8_t)(addr & 0xFF);
  buf[2] = (uint8_t)((addr >> 8) & 0xFF);
  buf[3] = (uint8_t)((addr >> 16) & 0xFF);
  buf[4] = (uint8_t)((addr >> 24) & 0xFF);
  int r = ctrl_out(h, DFU_DNLOAD, 0, buf, sizeof(buf));
  if(r < 0)
    return r;
  return dfu_wait_while_busy(h);
}


static int
read_blocks(libusb_device_handle *h, uint32_t base_addr, uint8_t* out, size_t len)
{
  int block = 2; // data blocks start at 2
  size_t off = 0;

  if(ctrl_out(h, DFU_ABORT, 0, NULL, 0)) {
    printf("failed to abort\n");
    return -1;
  }

  if(set_address_pointer(h, base_addr) < 0) {
    printf("set address pointer 0x%x failed\n", base_addr);
    return -1;
  }

  if(ctrl_out(h, DFU_ABORT, 0, NULL, 0)) {
    printf("failed to abort\n");
    return -1;
  }

  int xfer_size = 1024;
  while(off < len) {
    int chunk = MIN(xfer_size, len - off);
    int r = ctrl_in(h, DFU_UPLOAD, block, out + off, chunk);
    if(r < 0) {
      printf("DFU_UPLOAD failed %d\n", r);
      return r;
    }
    off += chunk;
    block++;
  }
  return 0;
}


static int
is_ff(const uint8_t *p, size_t len)
{
  for(size_t i = 0 ; i < len; i++) {
    if(p[i] != 0xff)
      return 0;
  }
  return 1;
}

__attribute__((unused))
static int
write_blocks(libusb_device_handle *h, uint32_t base_addr, const uint8_t* data, size_t len,
             int xfer_size)
{
  int block = 0;
  size_t off = 0;

  int need_set_address = 1;

  while(off < len){
    int chunk = MIN(xfer_size, len - off);
    const uint8_t* p = data + off;

    if(chunk == xfer_size && is_ff(p, xfer_size)) {
      need_set_address = 1;

    } else {

      if(need_set_address) {

        if(ctrl_out(h, DFU_ABORT, 0, NULL, 0))
          return -1;

        if(set_address_pointer(h, base_addr + off) < 0)
          return -1;
        block = 2;
        need_set_address = 0;
        printf("%sWrite to 0x%zx ", off ? "\n" : "", base_addr + off);
        fflush(stdout);
      }

      int r = ctrl_out(h, DFU_DNLOAD, (uint16_t)block, p, chunk);
      if(r<0)
        return r;
      dfu_wait_while_busy(h);
      block++;
      printf(".");
      fflush(stdout);
    }
    off += chunk;
  }
  printf("\n");
  return 0;
}


__attribute__((unused))
static int
erase_address(libusb_device_handle *h, uint32_t addr)
{
  uint8_t buf[5];
  buf[0] = BL_CMD_ERASE;
  buf[1] = (uint8_t)(addr & 0xFF);
  buf[2] = (uint8_t)((addr >> 8) & 0xFF);
  buf[3] = (uint8_t)((addr >> 16) & 0xFF);
  buf[4] = (uint8_t)((addr >> 24) & 0xFF);
  int r = ctrl_out(h, DFU_DNLOAD, 0, buf, sizeof(buf));
  if(r<0) return r;
  return dfu_wait_while_busy(h);
}

__attribute__((unused))
static int
erase_mass(libusb_device_handle *h)
{
  uint8_t buf[1];
  buf[0] = BL_CMD_ERASE; // with data length == 1 => mass erase per AN3156
  int r = ctrl_out(h, DFU_DNLOAD, 0, buf, sizeof(buf));
  if(r < 0)
    return r;
  return dfu_wait_while_busy(h);
}



static int
get_dfu_interface_string(libusb_device_handle *h, char* out, size_t outlen,
                         int *xfer_size)
{
  struct libusb_config_descriptor *cfg = NULL;
  int r;
  libusb_device* dev = libusb_get_device(h);

  if((r = libusb_get_active_config_descriptor(dev, &cfg)) != 0 || !cfg) {
    printf("Unable to get active config descriptor\n");
    return -1;
  }

  int idx = -1;
  int found = 0;
  for(int ifc=0; ifc < cfg->bNumInterfaces; ifc++) {
    const struct libusb_interface* itf = &cfg->interface[ifc];
    for(int alt=0; alt<itf->num_altsetting; ++alt){
      const struct libusb_interface_descriptor* id = &itf->altsetting[alt];
      if(id->bInterfaceClass != 254)
        continue;
      if(id->bInterfaceSubClass != 1)
        continue;

      if(id->bInterfaceNumber == INTERFACE) {
        if(idx == -1)
          idx = id->iInterface;
      }

      const uint8_t *p   = id->extra;
      int            rem = id->extra_length;
      while (rem >= 2 && !found) {
        uint8_t bLength = p[0];
        uint8_t bType   = p[1];

        if (bLength == 0 || bLength > rem) break; // malformed; stop scanning this alt

        if (bType == DFU_FUNC_DESC && bLength >= DFU_FUNC_LEN_MIN) {
          const struct dfu_functional_descriptor *dfu =
            (const struct dfu_functional_descriptor *)p;

          *xfer_size = dfu->wTransferSize;
        }

        p   += bLength;
        rem -= bLength;
      }
    }
  }
  if(cfg)
    libusb_free_config_descriptor(cfg);

  if(idx <= 0) {
    printf("No flash geometry config descriptor found\n");
    return -1;
  }


  memset(out, 0, outlen);
  int n = libusb_get_string_descriptor_ascii(h, idx, (unsigned char*)out, outlen - 1);
  return n < 0;
}


/**
 Example geometries
 stm32h7: @Internal Flash   /0x08000000/8*128Kg
 stm32f4: @Internal Flash  /0x08000000/04*016Kg,01*064Kg,07*128Kg
 stm32g4: @Internal Flash   /0x08000000/64*02Kg
*/


static int
parse_geometry(libusb_device_handle *h, uint32_t *base,
               uint32_t *sectorsizes, int max_count,
               int *xfer_size)
{
  char geometry[256];
  if(get_dfu_interface_string(h, geometry, sizeof(geometry),
                              xfer_size)) {
    printf("Unable to get flash geometry\n");
    return 0;
  }

  const char *s = geometry;
  s = strchr(s, '/');
  if(s == NULL) {
    goto bad;
  }

  s++;
  char *endptr;
  *base = strtol(s, &endptr, 16);
  if(endptr == s)
    goto bad;
  s = strchr(endptr, '/');
  if(s == NULL)
    goto bad;
  s++;

  uint32_t count = 0;
  while(*s) {
    char *endptr;
    uint32_t num_sec = strtol(s, &endptr, 10);
    if(*endptr != '*')
      goto bad;
    s = endptr + 1;
    uint32_t sec_size = strtol(s, &endptr, 10);

    s = endptr;
    if(*s == 'K')
      sec_size *= 1024;
    else if(*s == 'M')
      sec_size *= 1024 * 1024;
    else
      goto bad;
    s++;
    while(*s && *s >= 'a' && *s <= 'z')
      s++;
    for(int i = 0; i < num_sec; i++) {
      if(count == max_count)
        goto bad;
      sectorsizes[count++] = sec_size;
    }
    if(*s == 0)
      break;
    if(*s == ',')
      s++;
    else
      goto bad;
  }
  return count;

 bad:
  printf("Unable to parse flash geometry: %s\n", geometry);
  return 0;
}



// Defined further down (byte-identical to mios src/util/crc32.c).
static uint32_t crc32(const void *data, size_t n);

// Build a cmdline blob and deposit it in device RAM at 'addr' via the DFU
// bootloader. Layout matches mios <mios/cmdline.h>:
//   [u32 magic][u32 length][string][u32 ~crc32(header+string)]
static int
write_cmdline(libusb_device_handle *h, uint32_t addr, uint32_t maxlen,
              const char *cmdline)
{
  size_t len = strlen(cmdline);
  if(len > maxlen) {
    printf("cmdline too long: %zu bytes (firmware max %u)\n", len, maxlen);
    return -1;
  }

  size_t total = 8 + len + 4;
  uint8_t *blob = calloc(1, total);
  uint32_t magic = 0x6c646d63; // CMDLINE_MAGIC ('cmdl')
  uint32_t length = len;
  memcpy(blob + 0, &magic, 4);
  memcpy(blob + 4, &length, 4);
  memcpy(blob + 8, cmdline, len);
  uint32_t crc = ~crc32(blob, 8 + len);
  memcpy(blob + 8 + len, &crc, 4);

  // ABORT + SET_ADDRESS_POINTER, then a single data block (block 2 writes
  // at the address pointer); the blob is far smaller than a transfer block.
  if(ctrl_out(h, DFU_ABORT, 0, NULL, 0) || set_address_pointer(h, addr) < 0) {
    free(blob);
    return -1;
  }

  int r = ctrl_out(h, DFU_DNLOAD, 2, blob, total);
  free(blob);
  if(r < 0 || dfu_wait_while_busy(h))
    return -1;

  printf("Wrote cmdline to 0x%08x: \"%s\"\n", addr, cmdline);
  return 0;
}

static int
stm32_dfu_flasher(const struct mios_image *mi,
                  libusb_device_handle *h,
                  int force_flash,
                  const char *cmdline)
{
  uint8_t buildid[20] = {};
  struct dfu_status st;

  if(dfu_getstatus(h, &st)) {
    printf("Failed to read status\n");
    return -1;
  }

  if(st.bState != DFU_STATE_dfuIDLE) {

    if(dfu_clrstatus(h)) {
      printf("Clear status failed\n");
      return -1;
    }

    if(dfu_getstatus(h, &st)) {
      printf("Failed to read status\n");
      return -1;
    }
  }

  if(ctrl_out(h, DFU_ABORT, 0, NULL, 0)) {
    printf("Unable to reset DFU statemachine\n");
    return -1;
  }

  if(read_blocks(h, mi->buildid_paddr, buildid, sizeof(buildid))) {
    printf("Failed to read current buildid at 0x%lx\n",
           (long)mi->buildid_paddr);
  }

  //  hexdump("CURRENT", buildid, sizeof(buildid));
  //  hexdump(" LOADED", mi->buildid, sizeof(mi->buildid));

  if(ctrl_out(h, DFU_ABORT, 0, NULL, 0)) {
    printf("Unable to reset DFU statemachine\n");
    return -1;
  }


  if(memcmp(buildid, mi->buildid, sizeof(buildid)) || force_flash) {
    int num_sector_sizes = 128;
    uint32_t flashstart;
    uint32_t sectorsizes[128];
    int xfer_size = 2048;

    num_sector_sizes = parse_geometry(h, &flashstart,
                                      sectorsizes, num_sector_sizes,
                                      &xfer_size);

    if(num_sector_sizes == 0) {
      printf("Mass erase ... ");
      fflush(stdout);
      erase_mass(h);
      printf("\n");
    } else {

      uint32_t erased_to = 0;
      int s = 0;
      while(mi->image_size > erased_to) {
        printf("Erasing sector %d\n", s);
        if(erase_address(h, flashstart + erased_to)) {
          printf("Erase failed\n");
          return -1;
        }
        erased_to += sectorsizes[s];
        s++;
      }
    }

    if(write_blocks(h, mi->load_addr, mi->image, mi->image_size,
                    xfer_size)) {
      printf("Write failed\n");
      return -1;
    }

  } else {
    printf("Build ID matches, skipping flash operation\n\n");
  }

  if(ctrl_out(h, DFU_ABORT, 0, NULL, 0)) {
    printf("Unable to reset DFU statemachine\n");
    return -1;
  }

  if(cmdline != NULL) {
    if(mi->cmdline_addr == 0) {
      printf("Firmware does not export a cmdline region; ignoring cmdline\n");
    } else if(write_cmdline(h, mi->cmdline_addr, mi->cmdline_size, cmdline)) {
      return -1;
    } else if(ctrl_out(h, DFU_ABORT, 0, NULL, 0)) {
      printf("Unable to reset DFU statemachine\n");
      return -1;
    }
  }

  if(set_address_pointer(h, mi->load_addr) < 0) {
    printf("Failed to set address pointer\n");
    return -1;
  }

  int r = ctrl_out(h, DFU_DNLOAD, 0, NULL, 0);
  if(r < 0) {
    printf("Unable to leave DFU\n");
    return -1;
  }
  printf("Leaving DFU, GLHF\n");
  dfu_getstatus(h, &st);
  return 0;
}


// Scan USB devices for a DFU Runtime interface.
// Returns a handle to the device (with the runtime interface claimed),
// or NULL if not found. Sets *iface_num to the interface number.
static libusb_device_handle *
find_dfu_runtime_device(libusb_context *ctx, int *iface_num)
{
  libusb_device **devlist;
  ssize_t cnt = libusb_get_device_list(ctx, &devlist);

  for(ssize_t i = 0; i < cnt; i++) {
    struct libusb_config_descriptor *cfg;
    if(libusb_get_active_config_descriptor(devlist[i], &cfg) != 0)
      continue;

    int found_iface = -1;
    int valid_func_desc = 0;

    for(int j = 0; j < cfg->bNumInterfaces && found_iface < 0; j++) {
      const struct libusb_interface *iface = &cfg->interface[j];
      for(int a = 0; a < iface->num_altsetting; a++) {
        const struct libusb_interface_descriptor *alt = &iface->altsetting[a];

        // DFU Runtime: class=0xFE, subclass=0x01, protocol=0x01
        if(alt->bInterfaceClass != 0xfe ||
           alt->bInterfaceSubClass != 0x01 ||
           alt->bInterfaceProtocol != 0x01)
          continue;

        // Validate DFU Functional Descriptor in extra bytes
        const uint8_t *p = alt->extra;
        int rem = alt->extra_length;
        while(rem >= 2) {
          uint8_t bLength = p[0];
          uint8_t bType = p[1];

          if(bLength == 0 || bLength > rem)
            break;

          if(bType == DFU_FUNC_DESC && bLength >= DFU_FUNC_LEN_MIN) {
            const struct dfu_functional_descriptor *dfu =
              (const struct dfu_functional_descriptor *)p;
            if(dfu->bcdDFU == 0x0110) {
              valid_func_desc = 1;
            }
          }
          p += bLength;
          rem -= bLength;
        }

        if(valid_func_desc) {
          found_iface = alt->bInterfaceNumber;
          break;
        }
      }
    }
    libusb_free_config_descriptor(cfg);

    if(found_iface >= 0) {
      libusb_device_handle *h;
      if(libusb_open(devlist[i], &h) == 0) {
        *iface_num = found_iface;
        libusb_free_device_list(devlist, 1);
        return h;
      }
    }
  }

  libusb_free_device_list(devlist, 1);
  return NULL;
}


// Send DFU_DETACH to a runtime device and wait for it to re-enumerate
// as the ST DFU bootloader.
static libusb_device_handle *
detach_and_wait(libusb_context *ctx, libusb_device_handle *rt_handle,
                int iface_num)
{
  printf("Found DFU Runtime device, sending DFU_DETACH...\n");

  libusb_detach_kernel_driver(rt_handle, iface_num);
  libusb_claim_interface(rt_handle, iface_num);

  // DFU_DETACH: bmRequestType=0x21 (host-to-device, class, interface)
  //             bRequest=0x00 (DFU_DETACH)
  //             wValue=timeout(ms), wIndex=interface, wLength=0
  int r = libusb_control_transfer(rt_handle,
                                  0x21,        // bmRequestType
                                  DFU_DETACH,  // bRequest
                                  1000,        // wValue (timeout ms)
                                  iface_num,   // wIndex
                                  NULL, 0,     // no data
                                  5000);       // USB timeout

  libusb_release_interface(rt_handle, iface_num);
  libusb_close(rt_handle);

  // The device resets as part of detach, so the control transfer
  // often fails with IO error or no-device. That's expected.
  if(r < 0 &&
     r != LIBUSB_ERROR_IO &&
     r != LIBUSB_ERROR_NO_DEVICE &&
     r != LIBUSB_ERROR_PIPE) {
    printf("DFU_DETACH failed: %s\n", libusb_error_name(r));
    return NULL;
  }

  printf("Waiting for device to re-enumerate in DFU mode...\n");

  // Poll for the ST DFU bootloader to appear
  for(int attempt = 0; attempt < 20; attempt++) {
    usleep(250000);
    libusb_device_handle *h =
      libusb_open_device_with_vid_pid(ctx, 0x483, 0xdf11);
    if(h != NULL) {
      printf("Device is now in DFU mode\n\n");
      return h;
    }
  }

  printf("Timeout waiting for DFU bootloader\n");
  return NULL;
}


// Open a DFU device — either already in bootloader mode, or by
// finding a running device with DFU Runtime and sending DFU_DETACH.
static libusb_device_handle *
dfu_open(libusb_context *ctx)
{
  // First, check if a device is already in DFU bootloader mode
  libusb_device_handle *h =
    libusb_open_device_with_vid_pid(ctx, 0x483, 0xdf11);

  if(h == NULL) {
    // Not in DFU mode — look for a running device with DFU Runtime interface
    int iface_num;
    libusb_device_handle *rt = find_dfu_runtime_device(ctx, &iface_num);
    if(rt != NULL) {
      h = detach_and_wait(ctx, rt, iface_num);
    }
  }

  return h;
}


// ========================================================================
// STM32N6 boot-ROM provisioning
//
// The STM32N6 has no internal flash. Its boot ROM is also 0483:df11, but
// presents an "@FSBL" DFU interface (vs "@Internal Flash"); it downloads an
// image into AXISRAM2 and branches to it. We build a bootstrap image
//
//   [STM2 header (0x400)] [.boot FSBL @ 0x400] [parked mIAP mios ELF]
//
// in the v2.3 format the ROM accepts (UM3234 / AN5275), download it over
// plain DFU 1.1, and detach so the ROM authenticates and branches to our
// FSBL; the FSBL's serial-boot branch then rescues the parked mios ELF from
// SRAM. No signing tool or crypto is needed: the open (CLOSED_UNLOCKED)
// chip does not verify the (zero) signature.
// ========================================================================

#define N6_FSBL_HEADER_TOTAL  0x400
#define N6_POST_HEADER_LENGTH 0x1a0
#define N6_BOOT_PADDR_LO      0x34180400u
#define N6_BOOT_PADDR_HI      0x34200000u
#define N6_DOWNLOAD_MAX       (512 * 1024)
#define N6_XFER_SIZE          1024
#define MIOS_APP_MAGIC        0x5041496d
#define MIOS_IMG_TRAILER      "mI0sIMG1"
#define PT_LOAD               1

// mios crc32 (byte-identical to src/util/crc32.c)
static uint32_t
crc32_for_byte(uint32_t r)
{
  for(int j = 0; j < 8; j++)
    r = (r & 1 ? 0 : 0xEDB88320u) ^ (r >> 1);
  return r ^ 0xFF000000u;
}

static uint32_t
crc32(const void *data, size_t n)
{
  static uint32_t tbl[256];
  static int init;

  if(!init) {
    for(int i = 0; i < 256; i++)
      tbl[i] = crc32_for_byte(i);
    init = 1;
  }

  uint32_t crc = 0;
  const uint8_t *d = data;
  for(size_t i = 0; i < n; i++)
    crc = tbl[(uint8_t)crc ^ d[i]] ^ (crc >> 8);
  return crc;
}

typedef struct {
  uint8_t  e_ident[16];
  uint16_t e_type;
  uint16_t e_machine;
  uint32_t e_version;
  uint32_t e_entry;
  uint32_t e_phoff;
  uint32_t e_shoff;
  uint32_t e_flags;
  uint16_t e_ehsize;
  uint16_t e_phentsize;
  uint16_t e_phnum;
  uint16_t e_shentsize;
  uint16_t e_shnum;
  uint16_t e_shstrndx;
} Elf32_Ehdr;

typedef struct {
  uint32_t p_type;
  uint32_t p_offset;
  uint32_t p_vaddr;
  uint32_t p_paddr;
  uint32_t p_filesz;
  uint32_t p_memsz;
  uint32_t p_flags;
  uint32_t p_align;
} Elf32_Phdr;

// STM32N6 FSBL "STM2" header (UM3234 Table 32; mirrors stm32n6_flash.c).
struct stm32n6_fsbl_header {
  uint8_t  magic[4];
  uint8_t  signature[96];
  uint32_t image_checksum;
  uint32_t header_version;
  uint32_t image_length;
  uint32_t entry_point;
  uint32_t reserved1;
  uint32_t load_address;
  uint32_t reserved2;
  uint32_t version_number;
  uint32_t extension_flags;
  uint32_t post_header_length;
  uint32_t binary_type;
  uint8_t  pad[8];
  uint32_t ns_payload_length;
  uint32_t ns_payload_hash;
};

// Build the bootstrap image from a mios ELF. The STM2 header is the v2.3
// format STM32_SigningTool_CLI -hv 2.3 -align emits for an open chip: a 0xa0
// base header + a 0x1a0 padding extension + zero fill out to 0x400, so the
// FSBL code lands at 0x34180400 (= entry_point). The signature is left zero
// and the padding extension content is don't-care. image_length is measured
// from the end of header+extensions (0x240), so it includes the zero align
// gap before the code. The parked image is the whole ELF in the mios
// on-flash app format ([magic][length][ELF][~crc32][trailer]), placed right
// after .boot for the FSBL's serial-boot branch to rescue from SRAM.
static uint8_t *
n6_build_image(const char *elf_path, const char *cmdline, size_t *out_len)
{
  FILE *f = fopen(elf_path, "rb");
  if(f == NULL) {
    printf("Cannot open %s\n", elf_path);
    return NULL;
  }

  fseek(f, 0, SEEK_END);
  long elf_len = ftell(f);
  fseek(f, 0, SEEK_SET);

  uint8_t *elf = malloc(elf_len);
  if(fread(elf, 1, elf_len, f) != (size_t)elf_len) {
    fclose(f);
    free(elf);
    return NULL;
  }
  fclose(f);

  if(memcmp(elf, "\x7f""ELF", 4)) {
    printf("Not an ELF: %s\n", elf_path);
    free(elf);
    return NULL;
  }
  const Elf32_Ehdr *eh = (const Elf32_Ehdr *)elf;

  // Locate the .boot PT_LOAD (paddr in the download-buffer FSBL range).
  uint32_t boot_off = 0;
  uint32_t boot_sz = 0;
  for(int i = 0; i < eh->e_phnum; i++) {
    const Elf32_Phdr *ph =
      (const Elf32_Phdr *)(elf + eh->e_phoff + i * eh->e_phentsize);
    if(ph->p_type == PT_LOAD &&
       ph->p_paddr >= N6_BOOT_PADDR_LO && ph->p_paddr < N6_BOOT_PADDR_HI) {
      boot_off = ph->p_offset;
      boot_sz = ph->p_filesz;
    }
  }
  if(boot_sz == 0) {
    printf("No .boot segment (paddr 0x%x) in %s\n",
           N6_BOOT_PADDR_LO, elf_path);
    free(elf);
    return NULL;
  }

  uint32_t boot_padded = (boot_sz + 31) & ~31u;
  uint32_t stored_crc = ~crc32(elf, elf_len);
  size_t miap_len = 8 + elf_len + 4 + 8;

  // Boot cmdline blob ([magic][len][str][~crc32]), 32-byte aligned after the
  // parked mIAP app. The FSBL relocates it to the reserved RAM region.
  size_t cmd_len = cmdline ? strlen(cmdline) : 0;
  size_t cmdline_total = 8 + cmd_len + 4;
  size_t cmdline_off =
    (N6_FSBL_HEADER_TOTAL + boot_padded + miap_len + 31) & ~31u;

  size_t payload =
    ((cmdline_off - N6_FSBL_HEADER_TOTAL) + cmdline_total + 31) & ~31u;
  size_t total = N6_FSBL_HEADER_TOTAL + payload;

  if(total > N6_DOWNLOAD_MAX) {
    printf("Bootstrap image %zu bytes exceeds %d KB download buffer\n",
           total, N6_DOWNLOAD_MAX / 1024);
    free(elf);
    return NULL;
  }

  uint8_t *blob = calloc(1, total);

  struct stm32n6_fsbl_header *hd = (struct stm32n6_fsbl_header *)blob;
  hd->magic[0] = 'S';
  hd->magic[1] = 'T';
  hd->magic[2] = 'M';
  hd->magic[3] = 0x32;
  hd->header_version = 0x00020300;
  hd->image_length =
    (N6_FSBL_HEADER_TOTAL - sizeof(*hd) - N6_POST_HEADER_LENGTH) + payload;
  hd->entry_point = N6_BOOT_PADDR_LO + 1;   // .boot, +1 for Thumb
  hd->load_address = 0xFFFFFFFF;            // run in place in the buffer
  hd->extension_flags = 0x80000000;         // padding extension present
  hd->post_header_length = N6_POST_HEADER_LENGTH;
  hd->binary_type = 0x10;                   // FSBL

  // Padding extension at 0xa0 (type 'S','T',0xFF,0xFF).
  uint8_t *pext = blob + sizeof(*hd);
  pext[0] = 'S';
  pext[1] = 'T';
  pext[2] = 0xFF;
  pext[3] = 0xFF;
  uint32_t pext_len = N6_POST_HEADER_LENGTH;
  memcpy(pext + 4, &pext_len, 4);

  // FSBL .boot at file offset 0x400 -> SRAM 0x34180400.
  memcpy(blob + N6_FSBL_HEADER_TOTAL, elf + boot_off, boot_sz);

  // Parked mIAP image right after the 32-byte-padded .boot.
  uint8_t *p = blob + N6_FSBL_HEADER_TOTAL + boot_padded;
  uint32_t magic = MIOS_APP_MAGIC;
  uint32_t length = elf_len + 12;
  memcpy(p, &magic, 4);
  memcpy(p + 4, &length, 4);
  memcpy(p + 8, elf, elf_len);
  memcpy(p + 8 + elf_len, &stored_crc, 4);
  memcpy(p + 8 + elf_len + 4, MIOS_IMG_TRAILER, 8);

  // Boot cmdline blob (see cmdline_off above). An empty string still yields a
  // valid blob; mios cmdline_init rejects length 0 as "no cmdline".
  uint8_t *c = blob + cmdline_off;
  uint32_t cmd_magic = 0x6c646d63; // CMDLINE_MAGIC ('cmdl')
  uint32_t cmd_length = cmd_len;
  memcpy(c + 0, &cmd_magic, 4);
  memcpy(c + 4, &cmd_length, 4);
  if(cmd_len)
    memcpy(c + 8, cmdline, cmd_len);
  uint32_t cmd_crc = ~crc32(c, 8 + cmd_len);
  memcpy(c + 8 + cmd_len, &cmd_crc, 4);

  // Byte-sum checksum over the image region. The align gaps are zero, so this
  // equals the sum over the payload.
  uint32_t sum = 0;
  for(size_t i = 0; i < payload; i++)
    sum += blob[N6_FSBL_HEADER_TOTAL + i];
  hd->image_checksum = sum;

  printf("N6 bootstrap image: %zu bytes (.boot %u + parked mIAP %zu + cmdline %zu)\n",
         total, boot_sz, miap_len, cmdline_total);
  free(elf);
  *out_len = total;
  return blob;
}

// Wait out dfuDNBUSY after a DNLOAD. Returns 0 on idle with status OK, 1 if
// the ROM stopped ACKing (it auto-completed at header+image_length), or -1.
static int
n6_wait_idle(libusb_device_handle *h)
{
  for(int i = 0; i < 1000; i++) {
    struct dfu_status st;
    int r = libusb_control_transfer(h, CTRL_IN, DFU_GETSTATUS, 0, INTERFACE,
                                    (uint8_t *)&st, sizeof(st), 5000);
    if(r == LIBUSB_ERROR_TIMEOUT)
      return 1;
    if(r != (int)sizeof(st))
      return -1;
    if(st.bState == DFU_STATE_dfuDNBUSY) {
      usleep(get_poll_timeout(&st) * 1000);
      continue;
    }
    if(st.bStatus != DFU_STATUS_OK) {
      printf("DFU error: %s (state=%s)\n",
             dfu_status_str(st.bStatus), dfu_state_str(st.bState));
      return -1;
    }
    return 0;
  }
  return -1;
}

// Download the bootstrap image to the FSBL phase and branch to it. Plain
// DFU 1.1: sequential blocks from 0, zero-length DNLOAD to finish, then
// DFU_DETACH and poll GETSTATUS until the device stops ACKing. That
// disconnect is the ROM authenticating and branching to the FSBL (matching
// STM32CubeProgrammer's DfuDetach: no host USB reset, no alt switch).
static int
n6_download(libusb_device_handle *h, const uint8_t *blob, size_t len)
{
  struct dfu_status st;

  if(dfu_getstatus(h, &st) == 0 && st.bState == DFU_STATE_dfuERROR)
    dfu_clrstatus(h);
  ctrl_out(h, DFU_ABORT, 0, NULL, 0);

  // Select the @FSBL phase (alt 0); this also resets the DFU block counter.
  if(libusb_set_interface_alt_setting(h, INTERFACE, 0)) {
    printf("Failed to select FSBL phase (alt 0)\n");
    return -1;
  }

  printf("Downloading %zu bytes...\n", len);
  size_t off = 0;
  uint16_t block = 0;
  while(off < len) {
    int chunk = len - off < N6_XFER_SIZE ? (int)(len - off) : N6_XFER_SIZE;
    int r = libusb_control_transfer(h, CTRL_OUT, DFU_DNLOAD, block, INTERFACE,
                                    (uint8_t *)blob + off, chunk, 5000);
    if(r != chunk) {
      printf("DNLOAD block %u failed: %s\n", block,
             r < 0 ? libusb_error_name(r) : "short");
      return -1;
    }
    int w = n6_wait_idle(h);
    if(w < 0)
      return -1;
    off += chunk;
    block++;
    if(w == 1)  // ROM auto-completed at header+image_length
      break;
  }

  // End-of-download: zero-length DNLOAD, then poll manifestation to idle.
  libusb_control_transfer(h, CTRL_OUT, DFU_DNLOAD, block, INTERFACE,
                          NULL, 0, 2000);
  for(int i = 0; i < 50; i++) {
    if(dfu_getstatus(h, &st))
      break;
    if(st.bState == DFU_STATE_dfuIDLE)
      break;
    usleep(get_poll_timeout(&st) * 1000 + 20000);
  }

  libusb_control_transfer(h, CTRL_OUT, DFU_DETACH, 1000, INTERFACE,
                          NULL, 0, 2000);
  for(int i = 0; i < 60; i++) {
    int r = libusb_control_transfer(h, CTRL_IN, DFU_GETSTATUS, 0, INTERFACE,
                                    (uint8_t *)&st, sizeof(st), 500);
    if(r < 0) {
      printf("Branched to FSBL (device detached)\n");
      return 0;
    }
    usleep(50000);
  }
  printf("Device still in DFU after detach; image rejected?\n");
  return -1;
}

static const char *
n6_provision(libusb_device_handle *h, const char *elf_path, const char *cmdline)
{
  printf("\nSTM32N6 boot ROM detected; provisioning via ROM DFU\n\n");

  size_t len;
  uint8_t *blob = n6_build_image(elf_path, cmdline, &len);
  if(blob == NULL)
    return "Failed to build STM32N6 bootstrap image";

  libusb_set_auto_detach_kernel_driver(h, 1);
  if(libusb_claim_interface(h, INTERFACE)) {
    free(blob);
    return "Failed to claim STM32N6 DFU interface";
  }

  int r = n6_download(h, blob, len);
  libusb_release_interface(h, INTERFACE);
  free(blob);
  return r ? "STM32N6 ROM DFU download failed" : NULL;
}


// Load an ELF file and flash it via DFU.
// Returns NULL on success, or a static error string on failure.
static const char *
dfu_flash_elf(libusb_context *ctx, const char *elf_path, int force_flash,
              const char *cmdline)
{
  libusb_device_handle *h = dfu_open(ctx);
  if(h == NULL)
    return "No DFU device found (neither bootloader nor runtime)";

  // Autodetect the flash mechanism from the DFU interface string: "@FSBL"
  // is the STM32N6 boot ROM (no internal flash), anything else is a normal
  // STM32 DFU bootloader writing internal flash.
  char iface_str[128] = "";
  int xfer_size = 0;
  get_dfu_interface_string(h, iface_str, sizeof(iface_str), &xfer_size);

  if(!strncmp(iface_str, "@FSBL", 5)) {
    const char *err = n6_provision(h, elf_path, cmdline);
    libusb_close(h);
    return err;
  }

  const char *err;
  mios_image_t *mi = mios_image_from_elf_file(elf_path, 0, 0, &err);
  if(mi == NULL) {
    libusb_close(h);
    return err;
  }

  printf("\nLoaded application: %s\n\n", mi->appname);

  libusb_claim_interface(h, 0);
  int r = stm32_dfu_flasher(mi, h, force_flash, cmdline);
  libusb_release_interface(h, 0);
  libusb_close(h);
  free(mi);
  return r ? "DFU flash operation failed" : NULL;
}
