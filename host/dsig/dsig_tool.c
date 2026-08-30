/*
 * dsig — host CLI for the DSIG bus.
 *
 *   dsig listen   [SIGNAL [MASK]]
 *       Subscribe and print frames. With no args, listens to everything.
 *
 *   dsig emit     SIGNAL [HEXDATA]
 *       Send one DSIG frame and exit.
 *
 *   dsig periodic SIGNAL REFRESH_MS [HEXDATA]
 *       Publish payload every REFRESH_MS ms until interrupted.
 *
 *   dsig log      TXID RXID
 *       Open a VLLP client on the (txid,rxid) DSIG signal pair and stream
 *       the device's log to stdout.
 *
 *   dsig term     TXID RXID [CHANNEL]
 *       Open a VLLP client and attach an interactive shell on CHANNEL
 *       (default "shell"). Exit with Ctrl-B.
 *
 *   dsig ota      TXID RXID ELFPATH
 *       Open a VLLP client on the "ota" service and push ELFPATH to the
 *       target, which reboots into it on success. With -f, pushes and
 *       reboots even if the target's build-id already matches ELFPATH
 *       (normally a no-op).
 *
 *   dsig chargen  TXID RXID [SECONDS]
 *       Open the "chargen" service and measure download throughput for
 *       SECONDS (default 5) or until the server closes.
 *
 *   dsig discard  TXID RXID [SECONDS]
 *       Open the "discard" service and measure upload throughput for
 *       SECONDS (default 5) or until the server closes.
 *
 * Common options (before the subcommand):
 *   -t TRANSPORT  'udp' (default), 'cansock', 'usb', or
 *                 'file:///path/to/socket' for an AF_UNIX datagram
 *                 socket (e.g. talking to fcmon's gateway locally --
 *                 no self-echo, unlike UDP multicast, but same-host
 *                 only)
 *   -g GROUP      udp multicast group  (default 239.255.213.22)
 *   -p PORT       udp port             (default 0xd516)
 *   -i IFNAME     udp bind interface OR cansock ifname (default: any / can0)
 *   -m MTU        vllp mtu             (default 64, for FDCAN)
 *   -T SECONDS    vllp timeout         (default 3)
 *   -V VID:PID    usb vendor:product id, hex (default 6666:0, 0=any pid)
 *   -S SUBCLASS   usb-dsig vendor-interface subclass (default 2)
 *   -d            dump every raw TX/RX dsig frame to stderr
 *   -D ID[,ID...] signal ids known to carry VLLP link packets -- with
 *                 -d, decode them per docs/vllp.txt instead of hex
 *   -f            ota: force push+reboot even on a build-id match
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "dsig.h"
#include "dsig_cansock.h"
#include "dsig_udp.h"
#include "dsig_unix.h"
#include "dsig_usb.h"
#include "dsig_vllp.h"
#include "vllp.h"
#include "vllp_logstream.h"
#include "vllp_ota.h"
#include "vllp_term.h"

#include <ctype.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop;

static void
on_sigint(int s)
{
  (void)s;
  g_stop = 1;
}

static int
parse_hex_data(const char *s, uint8_t **outp, size_t *lenp)
{
  size_t slen = strlen(s);
  if(slen & 1)
    return -1;
  size_t n = slen / 2;
  uint8_t *buf = malloc(n ? n : 1);
  if(buf == NULL)
    return -1;
  for(size_t i = 0; i < n; i++) {
    char hex[3] = { s[2 * i], s[2 * i + 1], 0 };
    if(!isxdigit((unsigned char)hex[0]) || !isxdigit((unsigned char)hex[1])) {
      free(buf);
      return -1;
    }
    buf[i] = (uint8_t)strtoul(hex, NULL, 16);
  }
  *outp = buf;
  *lenp = n;
  return 0;
}

static void
print_signal(uint32_t signal, const void *data, size_t len)
{
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  printf("[%lld.%06ld] 0x%08" PRIx32 " (%zu)",
         (long long)ts.tv_sec, ts.tv_nsec / 1000, signal, len);
  const uint8_t *p = data;
  for(size_t i = 0; i < len; i++)
    printf(" %02x", p[i]);
  printf("\n");
  fflush(stdout);
}

static void
listen_cb(void *opaque, uint32_t signal, const void *data, size_t len)
{
  (void)opaque;
  if(data == NULL && len == 0)
    return;
  print_signal(signal, data, len);
}

/* -d: dump every raw dsig frame the transport actually sends/receives,
 * independent of whatever subcommand (ota/log/term/...) is running. */

static int g_debug;

/* -f: force ota to push and reboot even if the target's build-id
 * already matches ELFPATH (normally a no-op, useful for re-exercising
 * the actual bulk transfer, e.g. testing/repro without needing a real
 * code change to get a different build-id). */
static int g_force;

/* -D ID[,ID...]: signal ids known to carry VLLP link packets, so -d can
 * also decode them per docs/vllp.txt instead of just dumping hex. There
 * is no way to tell from the dsig frame alone that it's VLLP -- it's
 * just opaque bytes to the bus -- hence the explicit list. */

#define MAX_VLLP_IDS 8
static uint32_t g_vllp_ids[MAX_VLLP_IDS];
static int g_vllp_id_count;

static int
is_vllp_id(uint32_t signal)
{
  for(int i = 0; i < g_vllp_id_count; i++)
    if(g_vllp_ids[i] == signal)
      return 1;
  return 0;
}

// Best-effort VLLP link-packet decoder (docs/vllp.txt). Assumes the
// FDCAN padding adaptation is in effect (true whenever the session's
// mtu > 8, e.g. this tool's default -m 64): for logical frames > 8
// bytes, the trailing byte gives the total padding appended (including
// itself), used to round the wire length up to a valid FDCAN DLC.
static void
vllp_decode_dump(const void *data, size_t len)
{
  const uint8_t *p = data;
  size_t logical_len = len;
  if(len > 8) {
    uint8_t pad = p[len - 1];
    if(pad >= 1 && pad <= len)
      logical_len = len - pad;
  }

  if(logical_len == 0) {
    fprintf(stderr, "      [vllp: empty]\n");
    return;
  }

  const uint8_t hdr = p[0];
  const int seq  = (hdr >> 7) & 1;
  const int exp  = (hdr >> 6) & 1;
  const int flow = (hdr >> 5) & 1;
  const int last = (hdr >> 4) & 1;
  const int chan = hdr & 0xf;

  if(chan == 15) {
    if(!last) {
      // SYN: 0000_1111 [8bit version] [8bit mtu] [32bit cookie]
      if(logical_len >= 7) {
        uint32_t cookie = p[3] | (p[4] << 8) | (p[5] << 16) |
          ((uint32_t)p[6] << 24);
        fprintf(stderr, "      [vllp SYN version=%d mtu=%d cookie=0x%08x]\n",
               p[1], p[2], cookie);
      } else {
        fprintf(stderr, "      [vllp SYN (short, %zu bytes)]\n", logical_len);
      }
    } else {
      // ACK: SEF1_1111 [16bit flow-control bits] [32bit CRC]
      if(logical_len >= 7) {
        uint16_t flowbits = p[1] | (p[2] << 8);
        uint32_t crc = p[3] | (p[4] << 8) | (p[5] << 16) |
          ((uint32_t)p[6] << 24);
        fprintf(stderr,
               "      [vllp ACK seq=%d exp=%d flow_req=%d flowbits=0x%04x "
               "crc=0x%08x]\n", seq, exp, flow, flowbits, crc);
      } else {
        fprintf(stderr, "      [vllp ACK (short, %zu bytes)]\n", logical_len);
      }
    }
    return;
  }

  fprintf(stderr, "      [vllp seq=%d exp=%d flow=%d last=%d chan=%d",
         seq, exp, flow, last, chan);

  if(chan == 14) {
    // Channel Management Channel: OOOO_CCCC [opcode-specific payload],
    // +CRC32 if this is the last (here: only) fragment of the message.
    if(logical_len >= 2) {
      const uint8_t op_byte = p[1];
      const int opcode = (op_byte >> 4) & 0xf;
      const int target = op_byte & 0xf;
      const size_t crc_len = last ? 4 : 0;
      const uint8_t *payload = p + 2;
      const size_t hdr_len = 2 + crc_len;
      const size_t payload_len = logical_len >= hdr_len ?
        logical_len - hdr_len : 0;

      switch(opcode) {
      case 0:
        fprintf(stderr, " CMC open-request target=%d name=\"%.*s\"",
               target, (int)payload_len, payload);
        break;
      case 2: {
        uint16_t err = payload_len >= 2 ?
          (payload[0] | (payload[1] << 8)) : 0xffff;
        fprintf(stderr, " CMC open-response target=%d err=%d", target, err);
        break;
      }
      case 3: {
        uint16_t err = payload_len >= 2 ?
          (payload[0] | (payload[1] << 8)) : 0xffff;
        fprintf(stderr, " CMC close target=%d err=%d", target, err);
        break;
      }
      default:
        fprintf(stderr, " CMC opcode=%d target=%d", opcode, target);
        break;
      }
    }
  } else {
    const size_t crc_len = last ? 4 : 0;
    const size_t hdr_len = 1 + crc_len;
    const size_t payload_len = logical_len >= hdr_len ?
      logical_len - hdr_len : 0;
    fprintf(stderr, " data_len=%zu%s", payload_len, last ? " +crc32" : "");
  }

  fprintf(stderr, "]\n");
}

static void
dbg_dump(const char *dir, uint32_t signal, const void *data, size_t len)
{
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  fprintf(stderr, "[%lld.%06ld] %s 0x%08" PRIx32 " (%zu)",
         (long long)ts.tv_sec, ts.tv_nsec / 1000, dir, signal, len);
  const uint8_t *p = data;
  for(size_t i = 0; i < len; i++)
    fprintf(stderr, " %02x", p[i]);
  fprintf(stderr, "\n");
  if(is_vllp_id(signal))
    vllp_decode_dump(data, len);
}

static dsig_tx_fn g_real_tx;
static void *g_real_tx_opaque;

static void
debug_tx_thunk(void *opaque, uint32_t signal, const void *data, size_t len)
{
  (void)opaque;
  dbg_dump("TX", signal, data, len);
  g_real_tx(g_real_tx_opaque, signal, data, len);
}

static void
debug_rx_cb(void *opaque, uint32_t signal, const void *data, size_t len)
{
  (void)opaque;
  if(data == NULL && len == 0)
    return; // subscription timeout tick, not a real packet
  dbg_dump("RX", signal, data, len);
}

/* Transport plumbing */

typedef struct {
  dsig_udp_t *udp;
  dsig_cansock_t *can;
  dsig_usb_t *usbt;
  dsig_unix_t *unx;
} transport_t;

#define UNIX_SOCKET_PREFIX "file://"

static int
transport_open(transport_t *t, const char *kind,
               const char *group, uint16_t port, const char *ifname,
               uint16_t usb_vid, uint16_t usb_pid, uint8_t usb_subclass,
               dsig_t **out_bus)
{
  memset(t, 0, sizeof(*t));
  if(!strncmp(kind, UNIX_SOCKET_PREFIX, strlen(UNIX_SOCKET_PREFIX))) {
    const char *path = kind + strlen(UNIX_SOCKET_PREFIX);
    t->unx = dsig_unix_create(NULL, path);
    if(t->unx == NULL) {
      fprintf(stderr, "dsig: failed to open unix socket transport (%s)\n",
              path);
      return -1;
    }
    if(g_debug) {
      g_real_tx = dsig_unix_tx;
      g_real_tx_opaque = t->unx;
      *out_bus = dsig_create(debug_tx_thunk, NULL);
    } else {
      *out_bus = dsig_create(dsig_unix_tx, t->unx);
    }
    if(*out_bus == NULL) {
      dsig_unix_destroy(t->unx);
      return -1;
    }
    if(dsig_unix_start(t->unx, *out_bus) < 0) {
      dsig_destroy(*out_bus);
      dsig_unix_destroy(t->unx);
      return -1;
    }
    return 0;
  }
  if(!strcasecmp(kind, "udp")) {
    t->udp = dsig_udp_create(group, port, ifname);
    if(t->udp == NULL) {
      fprintf(stderr, "dsig: failed to open UDP transport\n");
      return -1;
    }
    if(g_debug) {
      g_real_tx = dsig_udp_tx;
      g_real_tx_opaque = t->udp;
      *out_bus = dsig_create(debug_tx_thunk, NULL);
    } else {
      *out_bus = dsig_create(dsig_udp_tx, t->udp);
    }
    if(*out_bus == NULL) {
      dsig_udp_destroy(t->udp);
      return -1;
    }
    if(dsig_udp_start(t->udp, *out_bus) < 0) {
      dsig_destroy(*out_bus);
      dsig_udp_destroy(t->udp);
      return -1;
    }
    return 0;
  }
  if(!strcasecmp(kind, "cansock") || !strcasecmp(kind, "can")) {
    t->can = dsig_cansock_create(ifname);
    if(t->can == NULL) {
      fprintf(stderr, "dsig: failed to open cansock transport (ifname=%s)\n",
              ifname ? ifname : "can0");
      return -1;
    }
    if(g_debug) {
      g_real_tx = dsig_cansock_tx;
      g_real_tx_opaque = t->can;
      *out_bus = dsig_create(debug_tx_thunk, NULL);
    } else {
      *out_bus = dsig_create(dsig_cansock_tx, t->can);
    }
    if(*out_bus == NULL) {
      dsig_cansock_destroy(t->can);
      return -1;
    }
    if(dsig_cansock_start(t->can, *out_bus) < 0) {
      dsig_destroy(*out_bus);
      dsig_cansock_destroy(t->can);
      return -1;
    }
    return 0;
  }
  if(!strcasecmp(kind, "usb")) {
    t->usbt = dsig_usb_create(usb_vid, usb_pid, usb_subclass);
    if(t->usbt == NULL) {
      fprintf(stderr, "dsig: failed to open USB transport\n");
      return -1;
    }
    if(g_debug) {
      g_real_tx = dsig_usb_tx;
      g_real_tx_opaque = t->usbt;
      *out_bus = dsig_create(debug_tx_thunk, NULL);
    } else {
      *out_bus = dsig_create(dsig_usb_tx, t->usbt);
    }
    if(*out_bus == NULL) {
      dsig_usb_destroy(t->usbt);
      return -1;
    }
    if(dsig_usb_start(t->usbt, *out_bus) < 0) {
      dsig_destroy(*out_bus);
      dsig_usb_destroy(t->usbt);
      return -1;
    }
    return 0;
  }
  fprintf(stderr, "dsig: unknown transport: %s\n", kind);
  return -1;
}

static void
transport_close(transport_t *t, dsig_t *bus)
{
  if(t->udp)  dsig_udp_destroy(t->udp);
  if(t->can)  dsig_cansock_destroy(t->can);
  if(t->usbt) dsig_usb_destroy(t->usbt);
  if(t->unx)  dsig_unix_destroy(t->unx);
  if(bus)     dsig_destroy(bus);
}

/* VLLP helpers */

static const char *
log_level_name(int level)
{
  switch(level & 7) {
  case LOG_EMERG:   return "EMERG";
  case LOG_ALERT:   return "ALERT";
  case LOG_CRIT:    return "CRIT";
  case LOG_ERR:     return "ERR";
  case LOG_WARNING: return "WARN";
  case LOG_NOTICE:  return "NOTICE";
  case LOG_INFO:    return "INFO";
  case LOG_DEBUG:   return "DEBUG";
  }
  return "?";
}

static void
on_log(void *opaque, int level, uint32_t seq, int64_t ms_ago, const char *msg)
{
  (void)opaque;
  (void)seq;
  printf("[%-6s -%5lld ms] %s\n", log_level_name(level),
         (long long)ms_ago, msg);
  fflush(stdout);
}

static void
on_vllp_log(void *opaque, int level, const char *msg)
{
  (void)opaque;
  fprintf(stderr, "vllp[%s]: %s\n", log_level_name(level), msg);
}

/* chargen/discard: minimal throughput/reliability tests for a raw VLLP
 * channel, independent of OTA-specific logic (flash writes, build-id
 * checks). chargen reads from the guest's 'chargen' service (which
 * streams at full speed, paced entirely by the channel's own flow
 * control since it's pull()-driven); discard writes to the guest's
 * 'discard' service.
 */

typedef struct {
  int64_t total;
  volatile int done;
} xfer_stats_t;

static void
xfer_rx_count(void *opaque, const void *data, size_t len)
{
  xfer_stats_t *xs = opaque;
  (void)data;
  xs->total += (int64_t)len;
}

static void
xfer_rx_ignore(void *opaque, const void *data, size_t len)
{
  (void)opaque; (void)data; (void)len;
}

static void
xfer_eof(void *opaque, int error_code)
{
  xfer_stats_t *xs = opaque;
  (void)error_code;
  xs->done = 1;
}

static double
elapsed_s(struct timespec *t0, struct timespec *t1)
{
  return (t1->tv_sec - t0->tv_sec) + (t1->tv_nsec - t0->tv_nsec) / 1e9;
}

static void
usage(void)
{
  fprintf(stderr,
"usage: dsig [GLOBAL OPTIONS] COMMAND [ARGS...]\n"
"\n"
"GLOBAL OPTIONS\n"
"  -t TRANSPORT   'udp' (default), 'cansock', 'usb', or\n"
"                 'file:///path/to/socket' (AF_UNIX datagram, same-host\n"
"                 only, no self-echo -- e.g. fcmon's gateway socket)\n"
"  -g GROUP       UDP multicast group       (default 239.255.213.22)\n"
"  -p PORT        UDP port                  (default 0xd516 = 54550)\n"
"  -i IFNAME      UDP bind interface, or cansock CAN ifname\n"
"                 (UDP default: any; cansock default: $IFC or 'can0')\n"
"  -m MTU         VLLP MTU                  (default 64; use 8 for legacy CAN)\n"
"  -T SECONDS     VLLP link timeout         (default 3)\n"
"  -V VID:PID     usb vendor:product id, hex (default 6666:0, 0=any pid)\n"
"  -S SUBCLASS    usb-dsig vendor-interface subclass (default 2)\n"
"  -d             Dump every raw TX/RX dsig frame (signal+hex) to stderr,\n"
"                 regardless of which command is running underneath.\n"
"  -D ID[,ID...]  Signal ids known to carry VLLP link packets -- with -d,\n"
"                 also decode them per docs/vllp.txt (SYN/ACK/CMC/data)\n"
"                 instead of just hex. Typically your TXID,RXID pair.\n"
"  -f             ota: force push+reboot even if the target's build-id\n"
"                 already matches ELFPATH (normally a no-op).\n"
"\n"
"Signal IDs accept C-style numeric literals (decimal, 0xHEX, 0OCT).\n"
"All TXID/RXID arguments are from the *host* point of view:\n"
"  TXID  - host transmits on this signal (device's rxid)\n"
"  RXID  - host receives from this signal (device's txid)\n"
"\n"
"DSIG COMMANDS (low-level pub/sub)\n"
"  listen [SIGNAL [MASK]]\n"
"      Subscribe and print every matching frame. With no arguments,\n"
"      prints everything (SIGNAL=0, MASK=0). Use MASK to widen the match:\n"
"      a sub matches when (received & MASK) == SIGNAL. Runs until Ctrl-C.\n"
"\n"
"  emit SIGNAL [HEXDATA]\n"
"      Send a single DSIG frame and exit. HEXDATA is an even-length hex\n"
"      string of payload bytes.\n"
"\n"
"  periodic SIGNAL REFRESH_MS [HEXDATA]\n"
"      Publish HEXDATA on SIGNAL every REFRESH_MS milliseconds.\n"
"      Runs until Ctrl-C.\n"
"\n"
"VLLP COMMANDS (run on top of a DSIG signal pair)\n"
"  log TXID RXID\n"
"      Open a VLLP client and stream the device's log to stdout via the\n"
"      'log' service.\n"
"\n"
"  term TXID RXID [CHANNEL]\n"
"      Open a VLLP client and attach an interactive shell on CHANNEL\n"
"      (default 'shell'). Exit with Ctrl-B.\n"
"\n"
"  ota TXID RXID ELFPATH\n"
"      Push ELFPATH to the target over the 'ota' service. The target\n"
"      reboots into it on success; a no-op if it's already running\n"
"      (unless -f is given, see GLOBAL OPTIONS).\n"
"\n"
"  chargen TXID RXID [SECONDS]\n"
"      Measure download throughput from the 'chargen' service for\n"
"      SECONDS (default 5) or until the server closes.\n"
"\n"
"  discard TXID RXID [SECONDS]\n"
"      Measure upload throughput to the 'discard' service for SECONDS\n"
"      (default 5) or until the server closes.\n"
"\n"
"EXAMPLES\n"
"  Watch every frame on the default UDP group:\n"
"      dsig listen\n"
"\n"
"  Send one frame on signal 0x1234:\n"
"      dsig emit 0x1234 deadbeef\n"
"\n"
"  Pretend to be a 100 Hz heartbeat publisher on signal 0x10:\n"
"      dsig periodic 0x10 10 01\n"
"\n"
"  Attach to the MTU-64 server on vexpress-a9 (device tx=0x200, rx=0x201):\n"
"      dsig term 0x201 0x200\n"
"\n"
"  Same server but stream logs only:\n"
"      dsig log 0x201 0x200\n"
"\n"
"  Same idea over real CAN with the MTU-8 server (device tx=0x210, rx=0x211):\n"
"      dsig -t cansock -i can0 -m 8 term 0x211 0x210\n"
"\n"
"  Push a firmware upgrade over real CAN-FD (device tx=0x210, rx=0x211):\n"
"      dsig -t cansock -i can0 -m 64 ota 0x211 0x210 build.myapp/myapp.elf\n");
}

int
main(int argc, char **argv)
{
  const char *transport = "udp";
  const char *group = NULL;
  uint16_t port = 0;
  const char *ifname = NULL;
  int mtu = 64;
  int timeout_s = 3;
  uint16_t usb_vid = 0x6666;
  uint16_t usb_pid = 0;
  uint8_t usb_subclass = 2;

  int opt;
  while((opt = getopt(argc, argv, "+t:g:p:i:m:T:V:S:D:dfh")) != -1) {
    switch(opt) {
    case 't': transport = optarg; break;
    case 'g': group = optarg; break;
    case 'p': port = (uint16_t)strtoul(optarg, NULL, 0); break;
    case 'i': ifname = optarg; break;
    case 'm': mtu = atoi(optarg); break;
    case 'T': timeout_s = atoi(optarg); break;
    case 'V': {
      char *colon = strchr(optarg, ':');
      usb_vid = (uint16_t)strtoul(optarg, NULL, 16);
      usb_pid = colon ? (uint16_t)strtoul(colon + 1, NULL, 16) : 0;
      break;
    }
    case 'S': usb_subclass = (uint8_t)strtoul(optarg, NULL, 0); break;
    case 'd': g_debug = 1; break;
    case 'f': g_force = 1; break;
    case 'D': {
      char *s = optarg;
      while(s != NULL && *s && g_vllp_id_count < MAX_VLLP_IDS) {
        g_vllp_ids[g_vllp_id_count++] = (uint32_t)strtoul(s, NULL, 0);
        s = strchr(s, ',');
        if(s != NULL)
          s++;
      }
      break;
    }
    case 'h':
    default: usage(); return opt == 'h' ? 0 : 2;
    }
  }

  if(optind >= argc) { usage(); return 2; }
  const char *cmd = argv[optind++];

  transport_t tr;
  dsig_t *bus = NULL;
  if(transport_open(&tr, transport, group, port, ifname,
                    usb_vid, usb_pid, usb_subclass, &bus) < 0)
    return 1;

  dsig_sub_t *dbg_sub = NULL;
  if(g_debug)
    dbg_sub = dsig_sub(bus, 0, 0, 0, debug_rx_cb, NULL);

  signal(SIGINT, on_sigint);
  signal(SIGTERM, on_sigint);

  int rc = 0;

  if(!strcasecmp(cmd, "listen")) {
    uint32_t sig = 0, mask = 0;
    if(optind < argc)
      sig = (uint32_t)strtoul(argv[optind++], NULL, 0);
    if(optind < argc)
      mask = (uint32_t)strtoul(argv[optind++], NULL, 0);
    dsig_sub(bus, sig, mask, 0, listen_cb, NULL);
    fprintf(stderr, "dsig: listening 0x%08x/0x%08x — Ctrl+C\n", sig, mask);
    while(!g_stop)
      pause();

  } else if(!strcasecmp(cmd, "emit")) {
    if(optind >= argc) { usage(); rc = 2; goto out; }
    uint32_t sig = (uint32_t)strtoul(argv[optind++], NULL, 0);
    uint8_t *data = NULL;
    size_t len = 0;
    if(optind < argc && parse_hex_data(argv[optind++], &data, &len) < 0) {
      fprintf(stderr, "dsig: invalid hex data\n");
      rc = 2; goto out;
    }
    dsig_send(bus, sig, data, len);
    free(data);
    usleep(20000);

  } else if(!strcasecmp(cmd, "periodic")) {
    if(optind + 1 >= argc) { usage(); rc = 2; goto out; }
    uint32_t sig = (uint32_t)strtoul(argv[optind++], NULL, 0);
    int refresh_ms = atoi(argv[optind++]);
    uint8_t *data = NULL;
    size_t len = 0;
    if(optind < argc && parse_hex_data(argv[optind++], &data, &len) < 0) {
      fprintf(stderr, "dsig: invalid hex data\n");
      rc = 2; goto out;
    }
    dsig_emitter_t *e = dsig_emitter_create(bus, sig, refresh_ms);
    if(e == NULL) {
      fprintf(stderr, "dsig: emitter_create failed\n");
      free(data); rc = 1; goto out;
    }
    dsig_emitter_update(e, data, len);
    free(data);
    fprintf(stderr, "dsig: publishing 0x%08x every %d ms — Ctrl+C\n",
            sig, refresh_ms);
    while(!g_stop)
      pause();
    dsig_emitter_destroy(e);

  } else if(!strcasecmp(cmd, "log")) {
    if(optind + 1 >= argc) { usage(); rc = 2; goto out; }
    uint32_t txid = (uint32_t)strtoul(argv[optind++], NULL, 0);
    uint32_t rxid = (uint32_t)strtoul(argv[optind++], NULL, 0);
    uint32_t flags = (mtu > 8) ? VLLP_FDCAN_ADAPTATION : 0;
    dsig_vllp_t *dv = dsig_vllp_client_create(bus, txid, rxid, mtu, timeout_s,
                                              flags, NULL, on_vllp_log);
    if(dv == NULL) {
      fprintf(stderr, "dsig: failed to create vllp client\n");
      rc = 1; goto out;
    }
    vllp_logstream_t *ls = vllp_logstream_create(dsig_vllp_get_vllp(dv),
                                                 NULL, on_log);
    if(ls == NULL) {
      fprintf(stderr, "dsig: logstream_create failed\n");
      dsig_vllp_destroy(dv);
      rc = 1; goto out;
    }
    fprintf(stderr, "dsig: streaming log (tx=0x%08x rx=0x%08x) — Ctrl+C\n",
            txid, rxid);
    while(!g_stop)
      pause();
    vllp_logstream_destroy(ls);
    dsig_vllp_destroy(dv);

  } else if(!strcasecmp(cmd, "term")) {
    if(optind + 1 >= argc) { usage(); rc = 2; goto out; }
    uint32_t txid = (uint32_t)strtoul(argv[optind++], NULL, 0);
    uint32_t rxid = (uint32_t)strtoul(argv[optind++], NULL, 0);
    const char *chan = optind < argc ? argv[optind++] : "shell";
    uint32_t flags = (mtu > 8) ? VLLP_FDCAN_ADAPTATION : 0;
    dsig_vllp_t *dv = dsig_vllp_client_create(bus, txid, rxid, mtu, timeout_s,
                                              flags, NULL, on_vllp_log);
    if(dv == NULL) {
      fprintf(stderr, "dsig: failed to create vllp client\n");
      rc = 1; goto out;
    }
    /* vllp_terminal() takes over stdin/stdout and exit()s when done. */
    vllp_terminal(dsig_vllp_get_vllp(dv), chan);
    dsig_vllp_destroy(dv);

  } else if(!strcasecmp(cmd, "ota")) {
    if(optind + 2 >= argc) { usage(); rc = 2; goto out; }
    uint32_t txid = (uint32_t)strtoul(argv[optind++], NULL, 0);
    uint32_t rxid = (uint32_t)strtoul(argv[optind++], NULL, 0);
    const char *elfpath = argv[optind++];
    uint32_t flags = (mtu > 8) ? VLLP_FDCAN_ADAPTATION : 0;
    dsig_vllp_t *dv = dsig_vllp_client_create(bus, txid, rxid, mtu, timeout_s,
                                              flags, NULL, on_vllp_log);
    if(dv == NULL) {
      fprintf(stderr, "dsig: failed to create vllp client\n");
      rc = 1; goto out;
    }
    const char *errstr = vllp_ota(dsig_vllp_get_vllp(dv), elfpath, g_force);
    if(errstr) {
      fprintf(stderr, "dsig: ota failed: %s\n", errstr);
      rc = 1;
    } else {
      fprintf(stderr, "dsig: ota complete (or already running)\n");
    }
    dsig_vllp_destroy(dv);

  } else if(!strcasecmp(cmd, "chargen")) {
    if(optind + 1 >= argc) { usage(); rc = 2; goto out; }
    uint32_t txid = (uint32_t)strtoul(argv[optind++], NULL, 0);
    uint32_t rxid = (uint32_t)strtoul(argv[optind++], NULL, 0);
    int duration_s = optind < argc ? atoi(argv[optind++]) : 5;
    uint32_t flags = (mtu > 8) ? VLLP_FDCAN_ADAPTATION : 0;
    dsig_vllp_t *dv = dsig_vllp_client_create(bus, txid, rxid, mtu, timeout_s,
                                              flags, NULL, on_vllp_log);
    if(dv == NULL) {
      fprintf(stderr, "dsig: failed to create vllp client\n");
      rc = 1; goto out;
    }
    xfer_stats_t xs = { 0 };
    vllp_channel_t *vc = vllp_channel_create(dsig_vllp_get_vllp(dv),
                                             "chargen", 0, xfer_rx_count,
                                             xfer_eof, NULL, &xs);
    if(vc == NULL) {
      fprintf(stderr, "dsig: failed to open chargen channel\n");
      dsig_vllp_destroy(dv);
      rc = 1; goto out;
    }
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    while(!xs.done && !g_stop) {
      usleep(20000);
      clock_gettime(CLOCK_MONOTONIC, &t1);
      if(elapsed_s(&t0, &t1) >= duration_s)
        break;
    }
    if(!xs.done)
      vllp_channel_close(vc, 0, 1);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double s = elapsed_s(&t0, &t1);
    fprintf(stderr, "dsig: chargen: %lld bytes in %.2fs (%.1f KB/s)%s\n",
           (long long)xs.total, s, xs.total / s / 1024.0,
           xs.done ? " -- server closed" : "");
    dsig_vllp_destroy(dv);

  } else if(!strcasecmp(cmd, "discard")) {
    if(optind + 1 >= argc) { usage(); rc = 2; goto out; }
    uint32_t txid = (uint32_t)strtoul(argv[optind++], NULL, 0);
    uint32_t rxid = (uint32_t)strtoul(argv[optind++], NULL, 0);
    int duration_s = optind < argc ? atoi(argv[optind++]) : 5;
    uint32_t flags = (mtu > 8) ? VLLP_FDCAN_ADAPTATION : 0;
    dsig_vllp_t *dv = dsig_vllp_client_create(bus, txid, rxid, mtu, timeout_s,
                                              flags, NULL, on_vllp_log);
    if(dv == NULL) {
      fprintf(stderr, "dsig: failed to create vllp client\n");
      rc = 1; goto out;
    }
    xfer_stats_t xs = { 0 };
    vllp_channel_t *vc = vllp_channel_create(dsig_vllp_get_vllp(dv),
                                             "discard", 0, xfer_rx_ignore,
                                             xfer_eof, NULL, &xs);
    if(vc == NULL) {
      fprintf(stderr, "dsig: failed to open discard channel\n");
      dsig_vllp_destroy(dv);
      rc = 1; goto out;
    }
    uint8_t buf[64];
    memset(buf, 0xa5, sizeof(buf));
    size_t chunk = (mtu > 0 && (size_t)mtu < sizeof(buf)) ?
      (size_t)mtu : sizeof(buf);

    // vllp_channel_send() has no backpressure signal (it just malloc()s
    // and queues), so pace ourselves rather than risk unbounded growth
    // if we can enqueue faster than the link can drain.
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    while(!xs.done && !g_stop) {
      vllp_channel_send(vc, buf, chunk);
      xs.total += (int64_t)chunk;
      usleep(1000);
      clock_gettime(CLOCK_MONOTONIC, &t1);
      if(elapsed_s(&t0, &t1) >= duration_s)
        break;
    }
    if(!xs.done)
      vllp_channel_close(vc, 0, 1);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double s = elapsed_s(&t0, &t1);
    fprintf(stderr, "dsig: discard: %lld bytes in %.2fs (%.1f KB/s)%s\n",
           (long long)xs.total, s, xs.total / s / 1024.0,
           xs.done ? " -- server closed" : "");
    dsig_vllp_destroy(dv);

  } else {
    fprintf(stderr, "dsig: unknown subcommand: %s\n", cmd);
    usage();
    rc = 2;
  }

out:
  if(dbg_sub)
    dsig_unsub(dbg_sub);
  transport_close(&tr, bus);
  return rc;
}
