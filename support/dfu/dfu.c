#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/param.h>

#include <libusb-1.0/libusb.h>

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
  int r = libusb_control_transfer(h, CTRL_IN, req, wValue, INTERFACE, buf, len, 5000);
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
    printf("set address pointer failed\n");
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
write_blocks(libusb_device_handle *h, uint32_t base_addr, const uint8_t* data, size_t len)
{
  int block = 0;
  size_t off = 0;

  int need_set_address = 1;
  const int xfer_size = 1024;

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
get_dfu_interface_string(libusb_device_handle *h, char* out, size_t outlen)
{
  struct libusb_config_descriptor *cfg = NULL;
  int r;
  libusb_device* dev = libusb_get_device(h);

  if((r = libusb_get_active_config_descriptor(dev, &cfg)) != 0 || !cfg) {
    printf("Unable to get active config descriptor\n");
    return -1;
  }

  int idx = -1;
  for(int ifc=0; ifc < cfg->bNumInterfaces; ifc++) {
    const struct libusb_interface* itf = &cfg->interface[ifc];
    for(int alt=0; alt<itf->num_altsetting; ++alt){
      const struct libusb_interface_descriptor* id = &itf->altsetting[alt];
      if(id->bInterfaceNumber == INTERFACE) {
        idx = id->iInterface;
        break;
      }
    }
    if(idx>=0)
      break;
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
               uint32_t *sectorsizes, int max_count)
{
  char geometry[256];
  if(get_dfu_interface_string(h, geometry, sizeof(geometry))) {
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



int
stm32_dfu_flasher(const struct mios_image *mi,
                  libusb_device_handle *h,
                  int force_flash)
{
  uint8_t buildid[20] = {};


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
    num_sector_sizes = parse_geometry(h, &flashstart,
                                      sectorsizes, num_sector_sizes);

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

    if(write_blocks(h, mi->load_addr, mi->image, mi->image_size)) {
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
  struct dfu_status st;
  dfu_getstatus(h, &st);
  return 0;
}


int
main(int argc, char **argv)
{
  libusb_context *ctx;

  if(argc != 2) {
    printf("usage: %s <elf-file>\n", argv[0]);
    exit(1);
  }

  const char *err;
  mios_image_t *mi = mios_image_from_elf_file(argv[1], 0, 0, &err);
  if(mi == NULL) {
    printf("Unable to load %s -- %s\n", argv[1], err);
    exit(1);
  }

  printf("\nLoaded application: %s\n\n", mi->appname);

  if(libusb_init(&ctx)) {
    printf("libusb_init failed\n");
    exit(1);
  }

  libusb_device_handle *h =
    libusb_open_device_with_vid_pid(ctx, 0x483, 0xdf11);

  if(h == NULL) {
    printf("Device in DFU not found\n");
    exit(1);
  }
  libusb_claim_interface(h, 0);

  int r = stm32_dfu_flasher(mi, h, 0);
  exit(!!r);
}
