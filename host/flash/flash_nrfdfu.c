// Self-contained Nordic USB DFU backend (nRF52840 Dongle and any board running
// the factory nRF Secure Bootloader). No external tools: it builds the DFU
// init packet (protobuf), computes the firmware SHA-256, and speaks the nRF DFU
// serial protocol (SLIP-framed) directly over the bootloader's CDC-ACM port.
//
// Reference for the on-wire format is the package nrfutil produces; the init
// packet layout and the byte-reversed firmware hash were reproduced from a
// known-good .dat.

#include "flash.h"
#include "mios_image.h"

#include <libusb.h>

#include <dirent.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

// --- Nordic USB identifiers ------------------------------------------------
#define NRF_VID_BOOTLOADER 0x1915
#define NRF_PID_BOOTLOADER 0x521f
#define MIOS_VID           0x6666

// DFU runtime interface (in the running app) used to reboot to the bootloader.
#define DFU_IFACE_CLASS    0xfe
#define DFU_IFACE_SUBCLASS 0x01
#define DFU_REQ_DETACH     0x00

// --- nRF DFU protocol ------------------------------------------------------
#define OP_CREATE       0x01
#define OP_SET_PRN      0x02
#define OP_CRC_GET      0x03
#define OP_EXECUTE      0x04
#define OP_SELECT       0x06
#define OP_MTU_GET      0x07
#define OP_WRITE        0x08
#define OP_RESPONSE     0x60
#define RES_SUCCESS     0x01

#define OBJ_COMMAND     0x01
#define OBJ_DATA        0x02

#define HW_VERSION      52  // nRF52


// ==========================================================================
// SHA-256 (self-contained)
// ==========================================================================
typedef struct {
  uint32_t s[8];
  uint64_t len;
  uint8_t buf[64];
  size_t n;
} sha256_t;

static uint32_t
ror(uint32_t x, int c)
{
  return (x >> c) | (x << (32 - c));
}

static void
sha256_block(sha256_t *c, const uint8_t *p)
{
  static const uint32_t k[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,
    0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
    0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,
    0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,
    0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
    0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,
    0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,
    0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
    0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
  uint32_t w[64];
  for(int i = 0; i < 16; i++)
    w[i] = (p[i*4] << 24) | (p[i*4+1] << 16) | (p[i*4+2] << 8) | p[i*4+3];
  for(int i = 16; i < 64; i++) {
    uint32_t s0 = ror(w[i-15],7) ^ ror(w[i-15],18) ^ (w[i-15] >> 3);
    uint32_t s1 = ror(w[i-2],17) ^ ror(w[i-2],19) ^ (w[i-2] >> 10);
    w[i] = w[i-16] + s0 + w[i-7] + s1;
  }
  uint32_t a=c->s[0],b=c->s[1],cc=c->s[2],d=c->s[3];
  uint32_t e=c->s[4],f=c->s[5],g=c->s[6],h=c->s[7];
  for(int i = 0; i < 64; i++) {
    uint32_t S1 = ror(e,6) ^ ror(e,11) ^ ror(e,25);
    uint32_t ch = (e & f) ^ (~e & g);
    uint32_t t1 = h + S1 + ch + k[i] + w[i];
    uint32_t S0 = ror(a,2) ^ ror(a,13) ^ ror(a,22);
    uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
    uint32_t t2 = S0 + maj;
    h=g; g=f; f=e; e=d+t1; d=cc; cc=b; b=a; a=t1+t2;
  }
  c->s[0]+=a; c->s[1]+=b; c->s[2]+=cc; c->s[3]+=d;
  c->s[4]+=e; c->s[5]+=f; c->s[6]+=g; c->s[7]+=h;
}

static void
sha256(const uint8_t *data, size_t len, uint8_t out[32])
{
  sha256_t c = { .s = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                       0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19} };
  c.len = len;
  while(len >= 64) { sha256_block(&c, data); data += 64; len -= 64; }
  uint8_t last[128];
  memcpy(last, data, len);
  last[len] = 0x80;
  size_t pad = (len < 56) ? 56 - len : 120 - len;
  memset(last + len + 1, 0, pad - 1);
  uint64_t bits = c.len * 8;
  for(int i = 0; i < 8; i++)
    last[len + pad + i] = bits >> (56 - i*8);
  sha256_block(&c, last);
  if(len + pad + 8 > 64)
    sha256_block(&c, last + 64);
  for(int i = 0; i < 8; i++) {
    out[i*4]   = c.s[i] >> 24;
    out[i*4+1] = c.s[i] >> 16;
    out[i*4+2] = c.s[i] >> 8;
    out[i*4+3] = c.s[i];
  }
}


// ==========================================================================
// CRC-32 (zlib/IEEE, chainable)
// ==========================================================================
static uint32_t
crc32z(uint32_t crc, const uint8_t *p, size_t n)
{
  crc = ~crc;
  for(size_t i = 0; i < n; i++) {
    crc ^= p[i];
    for(int k = 0; k < 8; k++)
      crc = (crc >> 1) ^ (0xedb88320u & -(crc & 1));
  }
  return ~crc;
}


// ==========================================================================
// DFU init packet (protobuf), reproducing nrfutil's layout
// ==========================================================================
static uint8_t *
pb_varint(uint8_t *p, uint32_t v)
{
  while(v >= 0x80) { *p++ = 0x80 | (v & 0x7f); v >>= 7; }
  *p++ = v;
  return p;
}

// Build the InitCommand body into buf; returns length.
static size_t
build_initcmd(uint8_t *buf, uint32_t fw_version, uint32_t app_size,
              const uint8_t hash_le[32])
{
  uint8_t *p = buf;
  p = pb_varint(p, 0x08); p = pb_varint(p, fw_version);   // 1 fw_version
  *p++ = 0x10; *p++ = HW_VERSION;                          // 2 hw_version
  *p++ = 0x1a; *p++ = 0x01; *p++ = 0x00;                   // 3 sd_req=[0]
  *p++ = 0x20; *p++ = 0x00;                                // 4 type=APPLICATION
  *p++ = 0x28; *p++ = 0x00;                                // 5 sd_size=0
  *p++ = 0x30; *p++ = 0x00;                                // 6 bl_size=0
  p = pb_varint(p, 0x38); p = pb_varint(p, app_size);      // 7 app_size
  *p++ = 0x42; *p++ = 0x24;                                // 8 hash (len 36)
  *p++ = 0x08; *p++ = 0x03;                                //   hash_type=SHA256
  *p++ = 0x12; *p++ = 0x20;                                //   hash bytes (32)
  memcpy(p, hash_le, 32); p += 32;
  *p++ = 0x48; *p++ = 0x00;                                // 9 is_debug=false
  *p++ = 0x52; *p++ = 0x04;                                // 10 boot_validation
  *p++ = 0x08; *p++ = 0x01;                                //    type=GENERATED_CRC
  *p++ = 0x12; *p++ = 0x00;                                //    bytes=empty
  return p - buf;
}

// Wrap the InitCommand in Command{op_code=INIT} and Packet{command}.
static size_t
build_init_packet(uint8_t *buf, uint32_t fw_version, uint32_t app_size,
                  const uint8_t hash_le[32])
{
  uint8_t init[128];
  size_t initlen = build_initcmd(init, fw_version, app_size, hash_le);

  uint8_t cmd[160];
  uint8_t *p = cmd;
  *p++ = 0x08; *p++ = 0x01;               // Command.op_code = INIT
  *p++ = 0x12; p = pb_varint(p, initlen); // Command.init
  memcpy(p, init, initlen); p += initlen;
  size_t cmdlen = p - cmd;

  uint8_t *q = buf;
  *q++ = 0x0a; q = pb_varint(q, cmdlen);  // Packet.command
  memcpy(q, cmd, cmdlen); q += cmdlen;
  return q - buf;
}


// ==========================================================================
// Serial transport (SLIP over the bootloader CDC-ACM tty)
// ==========================================================================
#define SLIP_END     0xc0
#define SLIP_ESC     0xdb
#define SLIP_ESC_END 0xdc
#define SLIP_ESC_ESC 0xdd

static int
slip_write(int fd, const uint8_t *data, size_t len)
{
  uint8_t out[1024];
  size_t o = 0;
  for(size_t i = 0; i < len; i++) {
    uint8_t b = data[i];
    if(o > sizeof(out) - 2)
      return -1;
    if(b == SLIP_END)      { out[o++] = SLIP_ESC; out[o++] = SLIP_ESC_END; }
    else if(b == SLIP_ESC) { out[o++] = SLIP_ESC; out[o++] = SLIP_ESC_ESC; }
    else                     out[o++] = b;
  }
  out[o++] = SLIP_END;
  size_t w = 0;
  while(w < o) {
    ssize_t r = write(fd, out + w, o - w);
    if(r < 0) return -1;
    w += r;
  }
  return 0;
}

// Read one SLIP frame with a timeout. Returns frame length or -1.
static int
slip_read(int fd, uint8_t *out, size_t max, int timeout_ms)
{
  size_t o = 0;
  int esc = 0;
  struct timespec deadline;
  clock_gettime(CLOCK_MONOTONIC, &deadline);
  uint64_t end = (uint64_t)deadline.tv_sec * 1000 + deadline.tv_nsec / 1000000
    + timeout_ms;
  for(;;) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t nowms = (uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
    if(nowms >= end)
      return -1;
    fd_set rf; FD_ZERO(&rf); FD_SET(fd, &rf);
    struct timeval tv = { .tv_sec = 0, .tv_usec = 20000 };
    if(select(fd + 1, &rf, NULL, NULL, &tv) <= 0)
      continue;
    uint8_t b;
    ssize_t r = read(fd, &b, 1);
    if(r <= 0)
      continue;
    if(esc) {
      b = (b == SLIP_ESC_END) ? SLIP_END : (b == SLIP_ESC_ESC) ? SLIP_ESC : b;
      esc = 0;
    } else if(b == SLIP_ESC) {
      esc = 1; continue;
    } else if(b == SLIP_END) {
      if(o == 0) continue; // leading/empty frame delimiter
      return o;
    }
    if(o < max) out[o++] = b;
  }
}

// Send a request and read the [0x60, op, result, ...] response. Returns the
// payload length (after the 3-byte header) or -1; payload copied to resp.
static int
dfu_request(int fd, flash_log_t *log, const uint8_t *req, size_t reqlen,
            uint8_t *resp, size_t respmax)
{
  if(slip_write(fd, req, reqlen))
    return -1;
  uint8_t buf[512];
  int n = slip_read(fd, buf, sizeof(buf), 5000);
  if(n < 3 || buf[0] != OP_RESPONSE || buf[1] != req[0]) {
    flash_logf(log, "DFU: bad response to op 0x%02x (n=%d)", req[0], n);
    return -1;
  }
  if(buf[2] != RES_SUCCESS) {
    flash_logf(log, "DFU: op 0x%02x failed, result 0x%02x", req[0], buf[2]);
    return -1;
  }
  int paylen = n - 3;
  if(resp && paylen > 0) {
    if((size_t)paylen > respmax) paylen = respmax;
    memcpy(resp, buf + 3, paylen);
  }
  return paylen;
}

static uint32_t rd32(const uint8_t *p)
{ return p[0] | (p[1]<<8) | (p[2]<<16) | ((uint32_t)p[3]<<24); }

static int
dfu_select(int fd, flash_log_t *log, uint8_t type, uint32_t *max_size)
{
  uint8_t req[2] = { OP_SELECT, type };
  uint8_t r[12];
  int n = dfu_request(fd, log, req, 2, r, sizeof(r));
  if(n < 12) return -1;
  *max_size = rd32(r);
  return 0;
}

static int
dfu_create(int fd, flash_log_t *log, uint8_t type, uint32_t size)
{
  uint8_t req[6] = { OP_CREATE, type, size, size>>8, size>>16, size>>24 };
  return dfu_request(fd, log, req, 6, NULL, 0);
}

static int
dfu_crc_get(int fd, flash_log_t *log, uint32_t *offset, uint32_t *crc)
{
  uint8_t req[1] = { OP_CRC_GET };
  uint8_t r[8];
  int n = dfu_request(fd, log, req, 1, r, sizeof(r));
  if(n < 8) return -1;
  *offset = rd32(r); *crc = rd32(r + 4);
  return 0;
}

static int
dfu_execute(int fd, flash_log_t *log)
{
  uint8_t req[1] = { OP_EXECUTE };
  return dfu_request(fd, log, req, 1, NULL, 0);
}

static int
dfu_set_prn(int fd, flash_log_t *log, uint16_t prn)
{
  uint8_t req[3] = { OP_SET_PRN, prn, prn >> 8 };
  return dfu_request(fd, log, req, 3, NULL, 0);
}

// Stream one object's payload as OBJECT_WRITE frames (no per-frame response).
static int
dfu_write_object(int fd, const uint8_t *data, size_t len)
{
  const size_t chunk = 64; // conservative; safe for any bootloader RX buffer
  uint8_t frame[1 + 64];
  for(size_t off = 0; off < len; off += chunk) {
    size_t n = len - off < chunk ? len - off : chunk;
    frame[0] = OP_WRITE;
    memcpy(frame + 1, data + off, n);
    if(slip_write(fd, frame, n + 1))
      return -1;
  }
  return 0;
}

// Transfer one logical stream (init packet or firmware) as a sequence of
// objects of at most max_size bytes, verifying the running CRC after each.
static int
dfu_stream(int fd, flash_log_t *log, uint8_t obj_type,
           const uint8_t *data, size_t len, uint32_t max_size)
{
  if(max_size == 0) max_size = len;
  uint32_t total = 0, running_crc = 0;
  for(size_t off = 0; off < len; off += max_size) {
    uint32_t n = len - off < max_size ? len - off : max_size;
    if(dfu_create(fd, log, obj_type, n) < 0)
      return -1;
    if(dfu_write_object(fd, data + off, n))
      return -1;
    running_crc = crc32z(running_crc, data + off, n);
    total += n;
    uint32_t roff, rcrc;
    if(dfu_crc_get(fd, log, &roff, &rcrc) < 0)
      return -1;
    if(roff != total || rcrc != running_crc) {
      flash_logf(log, "DFU: CRC mismatch (off %u/%u crc %08x/%08x)",
                 roff, total, rcrc, running_crc);
      return -1;
    }
    if(dfu_execute(fd, log) < 0)
      return -1;
  }
  return 0;
}


// ==========================================================================
// USB helpers: reboot the running app to its bootloader, find the tty
// ==========================================================================
static int
bootloader_present(libusb_context *usb, char *serial, size_t serialmax)
{
  libusb_device **list;
  ssize_t num = libusb_get_device_list(usb, &list);
  int found = 0;
  for(ssize_t i = 0; i < num && !found; i++) {
    struct libusb_device_descriptor d;
    if(libusb_get_device_descriptor(list[i], &d))
      continue;
    if(d.idVendor != NRF_VID_BOOTLOADER || d.idProduct != NRF_PID_BOOTLOADER)
      continue;
    found = 1;
    serial[0] = 0;
    libusb_device_handle *h;
    if(d.iSerialNumber && !libusb_open(list[i], &h)) {
      libusb_get_string_descriptor_ascii(h, d.iSerialNumber,
                                         (uint8_t *)serial, serialmax);
      libusb_close(h);
    }
  }
  libusb_free_device_list(list, 1);
  return found;
}

// Send DFU_DETACH to a running mios app so it reboots into the bootloader.
static void
detach_running_app(libusb_context *usb, flash_log_t *log)
{
  libusb_device **list;
  ssize_t num = libusb_get_device_list(usb, &list);
  for(ssize_t i = 0; i < num; i++) {
    struct libusb_device_descriptor d;
    if(libusb_get_device_descriptor(list[i], &d) || d.idVendor != MIOS_VID)
      continue;
    struct libusb_config_descriptor *cfg;
    if(libusb_get_active_config_descriptor(list[i], &cfg))
      continue;
    for(int j = 0; j < cfg->bNumInterfaces; j++) {
      const struct libusb_interface_descriptor *a =
        &cfg->interface[j].altsetting[0];
      if(a->bInterfaceClass != DFU_IFACE_CLASS ||
         a->bInterfaceSubClass != DFU_IFACE_SUBCLASS)
        continue;
      libusb_device_handle *h;
      if(libusb_open(list[i], &h))
        continue;
      libusb_claim_interface(h, a->bInterfaceNumber); // best effort
      flash_logf(log, "Rebooting app into bootloader (DFU detach)");
      libusb_control_transfer(h, 0x21, DFU_REQ_DETACH, 1000,
                              a->bInterfaceNumber, NULL, 0, 1000);
      libusb_release_interface(h, a->bInterfaceNumber);
      libusb_close(h);
    }
    libusb_free_config_descriptor(cfg);
  }
  libusb_free_device_list(list, 1);
}

// Find the bootloader's CDC tty (its name embeds the USB serial number).
static int
find_bootloader_tty(const char *serial, char *path, size_t pathmax)
{
  static const char *dirs[] = { "/dev", NULL };
  for(int di = 0; dirs[di]; di++) {
    DIR *d = opendir(dirs[di]);
    if(!d) continue;
    struct dirent *e;
    while((e = readdir(d))) {
      int is_cu = !strncmp(e->d_name, "cu.usbmodem", 11);      // macOS
      int is_acm = !strncmp(e->d_name, "ttyACM", 6);           // Linux
      if(!is_cu && !is_acm)
        continue;
      if(is_cu && serial[0] && !strstr(e->d_name, serial))
        continue;
      snprintf(path, pathmax, "%s/%s", dirs[di], e->d_name);
      closedir(d);
      return 0;
    }
    closedir(d);
  }
  return -1;
}

static int
serial_open(const char *path, flash_log_t *log)
{
  int fd = open(path, O_RDWR | O_NOCTTY);
  if(fd < 0) {
    flash_logf(log, "Cannot open %s", path);
    return -1;
  }
  struct termios t;
  tcgetattr(fd, &t);
  cfmakeraw(&t);
  t.c_cflag |= CLOCAL | CREAD;
  cfsetispeed(&t, B115200);
  cfsetospeed(&t, B115200);
  t.c_cc[VMIN] = 0;
  t.c_cc[VTIME] = 0;
  tcsetattr(fd, TCSANOW, &t);
  tcflush(fd, TCIOFLUSH);
  return fd;
}


// ==========================================================================
// Entry point
// ==========================================================================
int
flash_nrfdfu(const flash_params_t *p, flash_log_t *log)
{
  if(p->flags & FLASH_RESET_ONLY) {
    flash_logf(log, "Reset-only is not supported for nrfdfu");
    return -1;
  }

  const char *err = NULL;
  mios_image_t *img = mios_image_from_elf_file(p->elf_path, 0, 4, &err);
  if(img == NULL) {
    flash_logf(log, "ELF: %s", err ? err : "load failed");
    return -1;
  }

  uint8_t hash[32], hash_le[32];
  sha256(img->image, img->image_size, hash);
  for(int i = 0; i < 32; i++)          // nRF init packet stores the hash LE
    hash_le[i] = hash[31 - i];

  // fw_version must not regress across flashes; wall-clock seconds always does.
  uint32_t fw_version = (uint32_t)time(NULL);

  uint8_t initpkt[192];
  size_t initlen = build_init_packet(initpkt, fw_version,
                                     img->image_size, hash_le);

  libusb_context *usb;
  if(libusb_init(&usb)) {
    flash_logf(log, "libusb_init failed");
    free(img);
    return -1;
  }

  char serial[64] = {0};
  if(!bootloader_present(usb, serial, sizeof(serial))) {
    // Try to reboot a running mios app into its bootloader. This only works
    // where the bootloader honors the GPREGRET DFU-start flag; the PCA10059
    // Open Bootloader does not, and needs a physical RST (pin reset) instead.
    detach_running_app(usb, log);
    for(int i = 0; i < 30 && !bootloader_present(usb, serial, sizeof(serial));
        i++)
      usleep(100000); // up to 3s
  }
  if(!bootloader_present(usb, serial, sizeof(serial))) {
    flash_logf(log, "Waiting for bootloader: press the RST button on the "
               "dongle (red LED pulses). Up to 60s...");
    for(int i = 0; i < 600 && !bootloader_present(usb, serial, sizeof(serial));
        i++)
      usleep(100000);
  }
  if(!bootloader_present(usb, serial, sizeof(serial))) {
    flash_logf(log, "No nRF bootloader found.");
    libusb_exit(usb);
    free(img);
    return -1;
  }
  libusb_exit(usb);

  // Wait for the bootloader's serial port to appear.
  char tty[128];
  int found = 0;
  for(int i = 0; i < 50; i++) {
    if(!find_bootloader_tty(serial, tty, sizeof(tty))) { found = 1; break; }
    usleep(100000);
  }
  if(!found) {
    flash_logf(log, "Bootloader serial port did not appear");
    free(img);
    return -1;
  }
  flash_logf(log, "Bootloader on %s (serial %s)", tty,
             serial[0] ? serial : "?");

  int fd = serial_open(tty, log);
  if(fd < 0) { free(img); return -1; }

  int rc = -1;
  do {
    if(dfu_set_prn(fd, log, 0))
      break;

    // Init packet (command object).
    uint32_t max_cmd = 0;
    if(dfu_select(fd, log, OBJ_COMMAND, &max_cmd))
      break;
    if(initlen > max_cmd) {
      flash_logf(log, "Init packet too large (%zu > %u)", initlen, max_cmd);
      break;
    }
    flash_logf(log, "Sending init packet (%zu bytes)", initlen);
    if(dfu_stream(fd, log, OBJ_COMMAND, initpkt, initlen, max_cmd))
      break;

    // Firmware (data objects).
    uint32_t max_data = 0;
    if(dfu_select(fd, log, OBJ_DATA, &max_data))
      break;
    flash_logf(log, "Sending firmware (%zu bytes, object size %u)",
               img->image_size, max_data);
    if(dfu_stream(fd, log, OBJ_DATA, img->image, img->image_size, max_data))
      break;

    rc = 0;
  } while(0);

  close(fd);
  free(img);

  if(rc == 0)
    flash_logf(log, "nRF DFU flash successful; device rebooting");
  return rc;
}
