/*
 * ota: the full mios OTA flow, end to end, in virtual time.
 *
 * The real production host client (host/dsig/vllp.c, VLLP_SIM build) drives
 * the real mios OTA service (src/net/service/svc_ota.c) over a virtual CAN
 * link, and svc_ota streams the received image into a virtual SPI NOR flash
 * through the real production flash driver (src/drivers/spiflash.c). On a
 * successful transfer svc_ota reboots; on the host reboot() re-execs, so a
 * test hook (host_test_reboot_hook) intercepts it, and this suite then acts
 * as the bootloader: it re-reads the partition, validates the OTA header and
 * image CRC exactly as the platform bootloaders do, and confirms the stored
 * image is the intact firmware ELF that was sent.
 *
 * This mirrors the device flow:
 *   client -> VLLP -> svc_ota -> SPI flash partition ; reboot ; bootloader
 *                                                                validates.
 * The only thing not exercised is the literal jump into the new image (the
 * host can't execute a flat SPI image in place); the bootloader's accept
 * decision stands in for it.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include <mios/vllp.h>
#include <mios/block.h>
#include <mios/version.h>
#include <mios/error.h>

#include "util/crc32.h"

#include "hosttest.h"
#include "sim.h"
#include "vcan.h"
#include "drivers/spiflash.h"
#include "vspiflash.h"
#include "host_ota.h"
#include "cpu.h"
#include "irq.h"
#include "linux.h"
#include "../../../host/dsig/vllp_sim_api.h"

#define SEC 1000000ull

#define DEV_TXID 0x200
#define DEV_RXID 0x201
#define MTU 64

#define NOR_SIZE   (1u << 20)     // 1 MB virtual NOR
#define IMG_CAP    (256 * 1024)   // payload / reconstruction buffer cap

// OTA header written to block 0 of the partition (matches svc_ota / the
// platform bootloaders).
typedef struct {
  uint8_t magic[4];
  uint32_t size;
  uint32_t image_crc;
  uint32_t header_crc;
} otahdr_t;

typedef struct ota_scenario {
  vcan_t *vcan;
  struct spi *bus;     // virtual NOR, for fault injection
  hvllp_t *v;
  int failures;
  volatile int done;
  volatile int rebooted;

  uint8_t *elf;        // payload (the running ELF), padded with 0xff
  size_t elf_len;      // real length before padding
} ota_scenario_t;

static uint8_t g_elf[IMG_CAP];
static volatile int *g_rebooted_flag;

#define SCHECK(sc, cond, ...)                                           \
  do { if(!(cond)) { (sc)->failures++;                                  \
       hosttest_check(0, __FILE__, __LINE__, __VA_ARGS__); } } while(0)


// ---- reboot hook: stand in for the power-cycle into the bootloader ----

static void
ota_reboot_hook(void)
{
  if(g_rebooted_flag)
    *g_rebooted_flag = 1;
  // svc_ota did irq_forbid(IRQ_LEVEL_ALL) before rebooting; undo it so the
  // scheduler keeps running and the suite thread can verify the flash.
  irq_permit(0);
  while(1)
    usleep(1000000);   // park this (server) thread forever
}


// ---- transport hooks (run in the client sim thread) ----

static void
client_tx(void *opaque, const void *data, size_t len)
{
  ota_scenario_t *sc = opaque;
  vcan_peer_send(sc->vcan, DEV_RXID, data, len);
}

static long
client_recv(void *tr, uint32_t *id, void *buf, size_t buflen, int64_t deadline)
{
  ota_scenario_t *sc = tr;
  while(1) {
    long n = vcan_peer_recv(sc->vcan, id, buf, buflen, (uint64_t)deadline);
    if(n < 0)
      return -1;
    if(*id == DEV_TXID)
      return n;
  }
}

static void
client_log(void *opaque, int level, const char *msg)
{
  if(level <= 4 /* LOG_WARNING */)
    hosttest_log("  client WARN: %s", msg);
}


// ---- client helpers ----

static int
wait_connected(ota_scenario_t *sc, int64_t timeout_us)
{
  int64_t dl = clock_get() + timeout_us;
  while(!hvllp_is_connected(sc->v) && clock_get() < dl)
    hvllp_sim_poll(sc->v, dl);
  return hvllp_is_connected(sc->v);
}

// Pump until the channel's tx queue drains or the deadline passes. Returns
// the number of messages still pending (0 = fully drained). A non-zero
// return means the peer has stopped accepting (e.g. it rejected the upload
// and closed the channel mid-transfer).
static int
drain(ota_scenario_t *sc, hvllp_channel_t *ch)
{
  int64_t dl = clock_get() + 2 * SEC;
  while(hvllp_channel_tx_pending(ch) > 0 && clock_get() < dl)
    hvllp_sim_poll(sc->v, dl);
  return hvllp_channel_tx_pending(ch);
}

// Run one OTA upload. Returns the server's final status byte (0 = accepted),
// or a negative value on a protocol failure. If corrupt_crc is set the image
// CRC in the header is wrong, so the server must reject it.
static int
do_ota(ota_scenario_t *sc, int corrupt_crc, int graceful_close)
{
  hvllp_channel_t *ch =
    hvllp_channel_create(sc->v, "ota", 0, NULL, NULL, NULL, NULL);
  if(ch == NULL)
    return -1;

  // The server sends a running-info header on open: {0,'r',blocksize,skip},
  // then 20-byte build id, then app name.
  void *info = NULL;
  size_t ilen = 0;
  int err = hvllp_channel_read(ch, &info, &ilen, 5 * SEC);
  if(err || info == NULL || ilen < 4 + 20) {
    if(info) hvllp_sim_free(info);
    hvllp_channel_close(ch, 0, 1);
    return -2;   // (running-info failure: always close gracefully)
  }
  const uint8_t *h = info;
  int blocksize = h[2];
  int xferskip = h[3] * 1024;
  hvllp_sim_free(info);

  if(blocksize <= 0) {
    hvllp_channel_close(ch, 0, 1);
    return -3;
  }

  const uint8_t *src = sc->elf + xferskip;
  size_t src_len = sc->elf_len > (size_t)xferskip ? sc->elf_len - xferskip : 0;
  size_t nblocks = (src_len + blocksize - 1) / blocksize;
  size_t total = nblocks * blocksize;
  // src points into g_elf, whose tail is padded with 0xff, so a whole
  // blocksize can always be read for the final (partial) block.

  uint32_t crc = ~crc32(0, src, total);
  if(corrupt_crc)
    crc ^= 0xa5a5a5a5;

  uint8_t hdr[8];
  int32_t nb = (int32_t)nblocks;
  memcpy(hdr + 0, &nb, 4);
  memcpy(hdr + 4, &crc, 4);
  hvllp_channel_send(ch, hdr, sizeof(hdr));
  drain(sc, ch);

  for(size_t i = 0; i < nblocks; i++) {
    hvllp_channel_send(ch, src + i * blocksize, blocksize);
    if(drain(sc, ch) != 0)
      break;   // peer stopped accepting (rejected + closed mid-transfer)
  }

  void *fin = NULL;
  size_t flen = 0;
  err = hvllp_channel_read(ch, &fin, &flen, 30 * SEC);
  int status = (err == 0 && fin != NULL && flen == 1) ? ((uint8_t *)fin)[0] : -4;
  if(fin)
    hvllp_sim_free(fin);

  // On the golden (accepted) transfer the server is rebooting, so don't wait
  // on the close handshake; on a rejected transfer close gracefully so the
  // channel id is free before the next attempt reopens it.
  hvllp_channel_close(ch, 0, graceful_close);
  return status;
}


static void
ota_scenario_fn(void *arg)
{
  ota_scenario_t *sc = arg;

  hvllp_t *v = hvllp_create_client(MTU, 3, HVLLP_FDCAN_ADAPTATION, sc,
                                   client_tx, client_log);
  sc->v = v;
  hvllp_sim_setup(v, 0x07a5eed1, sc, client_recv, DEV_TXID);
  hvllp_start(v);

  if(!wait_connected(sc, 3 * SEC)) {
    SCHECK(sc, 0, "did not connect");
    sc->done = 1;
    return;
  }

  // Phase 1: a corrupt image must be rejected by svc_ota (CRC mismatch),
  // with a non-zero status and no reboot.
  hosttest_log("-- upload with bad CRC (must be rejected)");
  int s1 = do_ota(sc, 1, 1);
  SCHECK(sc, s1 > 0, "bad-crc: expected non-zero reject status, got %d", s1);
  SCHECK(sc, !sc->rebooted, "bad-crc: server rebooted on a corrupt image");
  SCHECK(sc, wait_connected(sc, 6 * SEC), "bad-crc: link did not survive");

  // Phase 1b: a flash that silently corrupts a written byte. svc_ota reads
  // every write back and CRCs it, so the corruption is caught end to end:
  // reject, no reboot, link stays alive (no wedge).
  hosttest_log("-- upload onto a flash that corrupts a byte (must be rejected)");
  vspiflash_corrupt_next_write(sc->bus);
  int sc1 = do_ota(sc, 0, 1);
  SCHECK(sc, sc1 > 0, "flash-corrupt: expected reject, got %d", sc1);
  SCHECK(sc, !sc->rebooted, "flash-corrupt: rebooted onto a bad image");
  SCHECK(sc, wait_connected(sc, 6 * SEC), "flash-corrupt: link did not survive");

  // Phase 1c: a flash that returns an I/O error partway through the write.
  // svc_ota must abort cleanly (non-zero status, no reboot) and recover.
  hosttest_log("-- upload onto a flash that errors mid-write (must fail cleanly)");
  vspiflash_fail_after(sc->bus, 8);   // 8 erase/write ops, then errors
  int sf1 = do_ota(sc, 0, 1);
  SCHECK(sc, sf1 > 0, "flash-ioerror: expected failure, got %d", sf1);
  SCHECK(sc, !sc->rebooted, "flash-ioerror: rebooted");
  vspiflash_fail_after(sc->bus, -1); // heal the flash
  SCHECK(sc, wait_connected(sc, 6 * SEC), "flash-ioerror: link did not survive");

  // Phase 2: with a healthy flash the real image is accepted, written, verified, and the server
  // reboots (intercepted by the hook).
  hosttest_log("-- upload real image (must be accepted -> reboot)");
  int s2 = do_ota(sc, 0, 0);
  SCHECK(sc, s2 == 0, "golden: expected status 0, got %d", s2);

  sc->done = 1;
}

static int
pred_rebooted(void *arg)
{
  ota_scenario_t *sc = arg;
  return sc->rebooted;
}


// ---- bootloader: validate the stored image, as the platform BLs do ----

static uint8_t g_recon[IMG_CAP];

// Read `total` bytes of the image (stored from 4 kB in) back through the
// real driver, block by block. Returns 0 on success.
static int
read_image(block_iface_t *bi, uint32_t total, uint8_t *dst)
{
  uint32_t off = 4096, pos = 0;
  while(pos < total) {
    uint32_t blk = off >> 12;
    uint32_t bo = off & 4095;
    uint32_t chunk = total - pos;
    if(bo + chunk > 4096)
      chunk = 4096 - bo;
    if(bi->read(bi, blk, bo, dst + pos, chunk))
      return -1;
    pos += chunk;
    off += chunk;
  }
  return 0;
}


static int
ota_bootloader_verify(ota_scenario_t *sc, block_iface_t *bi,
                      struct spi *bus)
{
  int fail = 0;

  otahdr_t hdr;
  error_t err = bi->read(bi, 0, 0, &hdr, sizeof(hdr));
  if(!CHECK(err == 0, "bootloader: header read: %s", error_to_string(err)))
    return fail + 1;

  fail += !CHECK(!memcmp(hdr.magic, "OTA1", 4), "bootloader: bad magic");
  uint32_t hcrc = ~crc32(0, &hdr, sizeof(hdr) - 4);
  fail += !CHECK(hcrc == hdr.header_crc,
                 "bootloader: header CRC 0x%08x != 0x%08x", hcrc, hdr.header_crc);

  fail += !CHECK(hdr.size >= sc->elf_len && hdr.size <= IMG_CAP,
                 "bootloader: implausible image size %u (elf %zu)",
                 hdr.size, sc->elf_len);
  uint32_t total = hdr.size <= IMG_CAP ? hdr.size : 0;

  fail += !CHECK(read_image(bi, total, g_recon) == 0,
                 "bootloader: image read failed");

  uint32_t icrc = ~crc32(0, g_recon, total);
  fail += !CHECK(icrc == hdr.image_crc,
                 "bootloader: image CRC 0x%08x != 0x%08x", icrc, hdr.image_crc);

  fail += !CHECK(total >= sc->elf_len &&
                 memcmp(g_recon, sc->elf, sc->elf_len) == 0,
                 "bootloader: stored image != source ELF");

  // The stored image is a valid mios ELF carrying the running build id.
  const unsigned char *bid = mios_build_id();
  int found = 0;
  if(total >= 20) {
    for(uint32_t i = 0; i + 20 <= total; i++) {
      if(memcmp(g_recon + i, bid, 20) == 0) { found = 1; break; }
    }
  }
  fail += !CHECK(found, "bootloader: build id not found in stored image");

  // A real bit rotting in the stored flash must be caught by the bootloader
  // CRC: poke one cell, read the image back through the driver, and confirm
  // the CRC no longer matches -- i.e. this image would be refused, not booted.
  if(total > 0) {
    vspiflash_poke(bus, 4096 + total / 2);
    if(read_image(bi, total, g_recon) == 0) {
      uint32_t bad = ~crc32(0, g_recon, total);
      fail += !CHECK(bad != hdr.image_crc,
                     "bootloader: CRC did not detect flash bit-rot");
    } else {
      fail++;
      CHECK(0, "bootloader: re-read after bit-rot failed");
    }
  }

  hosttest_log("  bootloader: OTA1 image %u bytes, CRC 0x%08x, build id "
               "%02x%02x%02x%02x... verified",
               hdr.size, hdr.image_crc, bid[0], bid[1], bid[2], bid[3]);
  return fail;
}


static size_t
read_self_exe(uint8_t *buf, size_t cap)
{
  long fd = linux_syscall(SYS_open, "/proc/self/exe", 0 /*O_RDONLY*/, 0);
  if(fd < 0)
    return 0;
  size_t n = 0;
  while(n < cap) {
    long r = linux_syscall(SYS_read, fd, buf + n, cap - n);
    if(r <= 0)
      break;
    n += r;
  }
  linux_syscall(SYS_close, fd);
  return n;
}


static int
test_ota(void)
{
  hosttest_log("---- OTA end to end: host client -> svc_ota -> SPI NOR ----");

  // Build the flash stack: virtual NOR chip <- real spiflash driver <-
  // partition, then point the "ota" service at it.
  struct spi *bus = vspiflash_create(NOR_SIZE);
  block_iface_t *flash = spiflash_create(bus, 0);
  if(!CHECK(flash != NULL, "spiflash_create failed"))
    return 1;
  block_iface_t *part =
    block_create_partition(flash, 0, flash->num_blocks, 0);
  host_ota_set_partition(part);

  // The payload is the running ELF; pad the tail with 0xff (erased flash).
  size_t n = read_self_exe(g_elf, IMG_CAP);
  if(!CHECK(n > 0 && n < IMG_CAP, "could not read /proc/self/exe (%zu)", n)) {
    host_ota_set_partition(NULL);
    return 1;
  }
  memset(g_elf + n, 0xff, IMG_CAP - n);

  vcan_t *vcan = vcan_create("vcan0", MTU);
  vllp_server_create(DEV_TXID, DEV_RXID, MTU, 3);
  vcan_set_link(vcan, 1);

  ota_scenario_t *sc = calloc(1, sizeof(*sc));
  sc->vcan = vcan;
  sc->bus = bus;
  sc->elf = g_elf;
  sc->elf_len = n;
  g_rebooted_flag = &sc->rebooted;
  host_test_reboot_hook = ota_reboot_hook;

  sim_thread_create("ota-client", ota_scenario_fn, sc, 1 << 20);

  CHECK(hosttest_wait(pred_rebooted, sc, 120 * SEC),
        "OTA did not complete and reboot");

  if(sc->rebooted)
    sc->failures += ota_bootloader_verify(sc, part, bus);

  host_test_reboot_hook = NULL;
  host_ota_set_partition(NULL);
  hosttest_log("  finished at t=%u us virtual time", (unsigned)clock_get());
  return sc->failures;
}

HOSTTEST_SUITE("ota", test_ota, 0);
