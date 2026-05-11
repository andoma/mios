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
 * Common options (before the subcommand):
 *   -t TRANSPORT  'udp' (default) or 'cansock'
 *   -g GROUP      udp multicast group  (default 239.255.213.22)
 *   -p PORT       udp port             (default 0xd516)
 *   -i IFNAME     udp bind interface OR cansock ifname (default: any / can0)
 *   -m MTU        vllp mtu             (default 64, for FDCAN)
 *   -T SECONDS    vllp timeout         (default 3)
 */

#define _GNU_SOURCE
#include "dsig.h"
#include "dsig_cansock.h"
#include "dsig_udp.h"
#include "dsig_vllp.h"
#include "vllp.h"
#include "vllp_logstream.h"
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

/* Transport plumbing */

typedef struct {
  dsig_udp_t *udp;
  dsig_cansock_t *can;
} transport_t;

static int
transport_open(transport_t *t, const char *kind,
               const char *group, uint16_t port, const char *ifname,
               dsig_t **out_bus)
{
  memset(t, 0, sizeof(*t));
  if(!strcasecmp(kind, "udp")) {
    t->udp = dsig_udp_create(group, port, ifname);
    if(t->udp == NULL) {
      fprintf(stderr, "dsig: failed to open UDP transport\n");
      return -1;
    }
    *out_bus = dsig_create(dsig_udp_tx, t->udp);
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
    *out_bus = dsig_create(dsig_cansock_tx, t->can);
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
  fprintf(stderr, "dsig: unknown transport: %s\n", kind);
  return -1;
}

static void
transport_close(transport_t *t, dsig_t *bus)
{
  if(t->udp) dsig_udp_destroy(t->udp);
  if(t->can) dsig_cansock_destroy(t->can);
  if(bus)    dsig_destroy(bus);
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

static void
usage(void)
{
  fprintf(stderr,
   "usage: dsig [-t TRANSPORT] [-g GROUP] [-p PORT] [-i IFNAME]\n"
   "            [-m MTU] [-T TIMEOUT] <subcommand> ...\n"
   "  listen   [SIGNAL [MASK]]\n"
   "  emit     SIGNAL [HEXDATA]\n"
   "  periodic SIGNAL REFRESH_MS [HEXDATA]\n"
   "  log      TXID RXID\n"
   "  term     TXID RXID [CHANNEL]\n");
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

  int opt;
  while((opt = getopt(argc, argv, "+t:g:p:i:m:T:h")) != -1) {
    switch(opt) {
    case 't': transport = optarg; break;
    case 'g': group = optarg; break;
    case 'p': port = (uint16_t)strtoul(optarg, NULL, 0); break;
    case 'i': ifname = optarg; break;
    case 'm': mtu = atoi(optarg); break;
    case 'T': timeout_s = atoi(optarg); break;
    case 'h':
    default: usage(); return opt == 'h' ? 0 : 2;
    }
  }

  if(optind >= argc) { usage(); return 2; }
  const char *cmd = argv[optind++];

  transport_t tr;
  dsig_t *bus = NULL;
  if(transport_open(&tr, transport, group, port, ifname, &bus) < 0)
    return 1;

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

  } else {
    fprintf(stderr, "dsig: unknown subcommand: %s\n", cmd);
    usage();
    rc = 2;
  }

out:
  transport_close(&tr, bus);
  return rc;
}
