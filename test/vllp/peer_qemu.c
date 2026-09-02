/*
 * QEMU peer: spawns qemu-system-arm running mios (vexpress-a9), talks
 * DSIG over UDP multicast through slirp's hostfwd, and drives the mios
 * CLI over the serial console (exposed as a TCP chardev) for server-side
 * introspection (show_vllp, show_malloc, PANIC detection).
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "peer.h"
#include "tst.h"
#include "dsig.h"
#include "dsig_udp.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define CONSOLE_BUF_SIZE (1 << 20)
#define PROMPT "[2K> "

typedef struct qemu_priv {
  peer_qemu_cfg_t cfg;
  pid_t pid;
  int console_fd;
  int console_port;
  FILE *console_log;

  pthread_t reader;
  int reader_run;
  pthread_mutex_t mutex;        /* console buffer */
  pthread_cond_t cond;
  char *buf;
  size_t buf_len;
  int panicked;

  pthread_mutex_t cmd_mutex;    /* one CLI command at a time */

  dsig_udp_t *udp;
  dsig_t *bus;
  dsig_sub_t *sub;

  pthread_mutex_t client_mutex;
} qemu_priv_t;

/* ---------------------------------------------------------------- */
/* Console                                                           */

static void *
console_reader(void *arg)
{
  peer_t *p = arg;
  qemu_priv_t *q = p->priv;
  char tmp[4096];
  while(q->reader_run) {
    ssize_t n = read(q->console_fd, tmp, sizeof(tmp));
    if(n <= 0) {
      if(n < 0 && (errno == EINTR || errno == EAGAIN))
        continue;
      break;
    }
    if(q->console_log) {
      fwrite(tmp, 1, n, q->console_log);
      fflush(q->console_log);
    }
    pthread_mutex_lock(&q->mutex);
    if(q->buf_len + n + 1 > CONSOLE_BUF_SIZE) {
      /* keep the tail */
      size_t keep = CONSOLE_BUF_SIZE / 2;
      memmove(q->buf, q->buf + q->buf_len - keep, keep);
      q->buf_len = keep;
    }
    memcpy(q->buf + q->buf_len, tmp, n);
    q->buf_len += n;
    q->buf[q->buf_len] = 0;
    if(!q->panicked && memmem(q->buf, q->buf_len, "PANIC:", 6) != NULL) {
      q->panicked = 1;
      tst_logf("%s: guest PANIC detected on console", p->name);
    }
    pthread_cond_broadcast(&q->cond);
    pthread_mutex_unlock(&q->mutex);
  }
  return NULL;
}

/* Wait until the console buffer (from offset 'from') contains 'needle'.
 * Returns pointer offset of the needle or -1 on timeout. */
static ssize_t
console_wait_for(qemu_priv_t *q, size_t from, const char *needle,
                 int64_t timeout_us)
{
  int64_t deadline = tst_now_us() + timeout_us;
  pthread_mutex_lock(&q->mutex);
  while(1) {
    if(q->buf_len > from) {
      char *hit = memmem(q->buf + from, q->buf_len - from, needle,
                         strlen(needle));
      if(hit) {
        ssize_t off = hit - q->buf;
        pthread_mutex_unlock(&q->mutex);
        return off;
      }
    }
    int64_t now = tst_now_us();
    if(now >= deadline)
      break;
    struct timespec ts = { deadline / 1000000, (deadline % 1000000) * 1000 };
    pthread_cond_timedwait(&q->cond, &q->mutex, &ts);
  }
  pthread_mutex_unlock(&q->mutex);
  return -1;
}

static void
strip_escapes(const char *in, size_t len, char *out, size_t outlen)
{
  size_t o = 0;
  for(size_t i = 0; i < len && o + 1 < outlen; i++) {
    if(in[i] == 0x1b) {
      /* CSI: ESC [ ... final byte in 0x40..0x7e */
      i++;
      if(i < len && in[i] == '[') {
        i++;
        while(i < len && !(in[i] >= 0x40 && in[i] <= 0x7e))
          i++;
      }
      continue;
    }
    if(in[i] == '\r')
      continue;
    out[o++] = in[i];
  }
  out[o] = 0;
}

static int
console_cmd(peer_t *p, const char *cmd, char *out, size_t outlen)
{
  qemu_priv_t *q = p->priv;
  if(q->console_fd < 0)
    return -1;

  pthread_mutex_lock(&q->cmd_mutex);
  pthread_mutex_lock(&q->mutex);
  size_t start = q->buf_len;
  pthread_mutex_unlock(&q->mutex);

  char line[256];
  int n = snprintf(line, sizeof(line), "%s\r", cmd);
  if(write(q->console_fd, line, n) != n) {
    pthread_mutex_unlock(&q->cmd_mutex);
    return -1;
  }

  /* The CLI echoes the command, then output, then a new prompt */
  ssize_t echo_end = console_wait_for(q, start, "\n", 3000000);
  if(echo_end < 0) {
    pthread_mutex_unlock(&q->cmd_mutex);
    return -1;
  }
  ssize_t prompt = console_wait_for(q, echo_end, PROMPT, 5000000);
  if(prompt < 0) {
    pthread_mutex_unlock(&q->cmd_mutex);
    return -1;
  }
  pthread_mutex_lock(&q->mutex);
  /* prompt points at "[2K> "; the ESC preceding it belongs to the prompt */
  size_t end = prompt;
  if(end > 0 && q->buf[end - 1] == 0x1b)
    end--;
  if(out)
    strip_escapes(q->buf + echo_end + 1, end - (echo_end + 1), out, outlen);
  pthread_mutex_unlock(&q->mutex);
  pthread_mutex_unlock(&q->cmd_mutex);
  return 0;
}

static int
console_connect(qemu_priv_t *q)
{
  int64_t deadline = tst_now_us() + 10000000;
  while(tst_now_us() < deadline) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa = { .sin_family = AF_INET,
                              .sin_port = htons(q->console_port) };
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if(connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0) {
      q->console_fd = fd;
      return 0;
    }
    close(fd);
    tst_sleep_us(100000);
  }
  return -1;
}

/* ---------------------------------------------------------------- */
/* Process                                                           */

static int
qemu_spawn(peer_t *p)
{
  qemu_priv_t *q = p->priv;
  char serial[128], hostfwd[128], stderr_path[512];
  snprintf(serial, sizeof(serial), "tcp:127.0.0.1:%d,server=on,wait=on",
           q->console_port);
  snprintf(hostfwd, sizeof(hostfwd), "user,hostfwd=udp::%d-:%d",
           q->cfg.udp_port, q->cfg.udp_port);
  snprintf(stderr_path, sizeof(stderr_path), "%s/qemu-stderr.log",
           q->cfg.logdir);

  pid_t pid = fork();
  if(pid < 0)
    return -1;
  if(pid == 0) {
    int devnull = open("/dev/null", O_RDONLY);
    dup2(devnull, 0);
    int err = open(stderr_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if(err >= 0) {
      dup2(err, 1);
      dup2(err, 2);
    }
    execlp(q->cfg.qemu_bin, q->cfg.qemu_bin,
           "-M", "vexpress-a9", "-m", "32M",
           "-display", "none", "-audio", "none", "-monitor", "none",
           "-serial", serial,
           "-kernel", q->cfg.firmware,
           "-net", "nic,model=lan9118",
           "-net", hostfwd,
           (char *)NULL);
    _exit(127);
  }
  q->pid = pid;
  return 0;
}

static void
qemu_kill(qemu_priv_t *q)
{
  if(q->pid <= 0)
    return;
  kill(q->pid, SIGTERM);
  int64_t deadline = tst_now_us() + 2000000;
  int status;
  while(tst_now_us() < deadline) {
    if(waitpid(q->pid, &status, WNOHANG) == q->pid) {
      q->pid = 0;
      return;
    }
    tst_sleep_us(20000);
  }
  kill(q->pid, SIGKILL);
  waitpid(q->pid, &status, 0);
  q->pid = 0;
}

/* ---------------------------------------------------------------- */
/* Data path                                                         */

static void
deliver_to_udp(void *opaque, const void *data, size_t len)
{
  peer_t *p = opaque;
  qemu_priv_t *q = p->priv;
  dsig_send(q->bus, q->cfg.txid, data, len);
}

static void
client_tx(void *opaque, const void *data, size_t len)
{
  peer_t *p = opaque;
  linksim_send(p->ls, PEER_DIR_C2S, data, len, deliver_to_udp, p);
}

static void
deliver_to_client(void *opaque, const void *data, size_t len)
{
  peer_t *p = opaque;
  qemu_priv_t *q = p->priv;
  pthread_mutex_lock(&q->client_mutex);
  if(p->v != NULL)
    vllp_input(p->v, data, len);
  pthread_mutex_unlock(&q->client_mutex);
}

static void
udp_rx(void *opaque, uint32_t signal, const void *data, size_t len)
{
  peer_t *p = opaque;
  (void)signal;
  if(data == NULL || len == 0)
    return;
  linksim_send(p->ls, PEER_DIR_S2C, data, len, deliver_to_client, p);
}

static vllp_t *
make_client(peer_t *p)
{
  vllp_t *v = vllp_create_client(p->mtu, p->timeout, p->vllp_flags, p,
                                 client_tx, peer_log_cb);
  vllp_start(v);
  return v;
}

/* ---------------------------------------------------------------- */
/* peer ops                                                          */

static int
qemu_restart_client(peer_t *p)
{
  qemu_priv_t *q = p->priv;
  pthread_mutex_lock(&q->client_mutex);
  vllp_t *old = p->v;
  p->v = NULL;
  pthread_mutex_unlock(&q->client_mutex);
  vllp_destroy(old);
  vllp_t *nv = make_client(p);
  pthread_mutex_lock(&q->client_mutex);
  p->v = nv;
  pthread_mutex_unlock(&q->client_mutex);
  return 0;
}

static int
qemu_server_status(peer_t *p, peer_server_status_t *st)
{
  qemu_priv_t *q = p->priv;
  memset(st, 0, sizeof(*st));
  st->panicked = q->panicked;
  if(st->panicked)
    return 0;

  char out[4096];
  if(console_cmd(p, "show_vllp", out, sizeof(out)) < 0) {
    snprintf(st->detail, sizeof(st->detail), "show_vllp: no response");
    return -1;
  }
  snprintf(st->detail, sizeof(st->detail), "%s", out);

  /* Find our server block: "TX:0x<device txid>  RX:0x<device rxid> ..." */
  char key[64];
  snprintf(key, sizeof(key), "TX:0x%x  RX:0x%x", q->cfg.rxid, q->cfg.txid);
  char *blk = strstr(out, key);
  if(blk == NULL) {
    snprintf(st->detail, sizeof(st->detail), "server %s not in show_vllp:\n%.3000s",
             key, out);
    return -1;
  }
  char *hdr_end = strchr(blk, '\n');
  if(hdr_end) {
    *hdr_end = 0;
    st->connected = strstr(blk, "Connected") != NULL &&
      strstr(blk, "Disconnected") == NULL;
    *hdr_end = '\n';
  }
  /* channel lines until blank line or next "TX:" */
  char *s = hdr_end ? hdr_end + 1 : blk;
  while(s && *s) {
    char *eol = strchr(s, '\n');
    size_t l = eol ? (size_t)(eol - s) : strlen(s);
    if(l == 0 || !strncmp(s, "TX:", 3))
      break;
    char *st_pos = memmem(s, l, ": state:", 8);
    if(st_pos) {
      int id = atoi(s);
      if(id != 14)
        st->user_channels++;
    }
    s = eol ? eol + 1 : NULL;
  }
  return 0;
}

static void
qemu_destroy(peer_t *p)
{
  qemu_priv_t *q = p->priv;

  pthread_mutex_lock(&q->client_mutex);
  vllp_t *old = p->v;
  p->v = NULL;
  pthread_mutex_unlock(&q->client_mutex);
  if(old)
    vllp_destroy(old);

  if(q->sub)
    dsig_unsub(q->sub);
  linksim_drain(p->ls, 1000000);
  if(q->udp)
    dsig_udp_destroy(q->udp);
  if(q->bus)
    dsig_destroy(q->bus);
  linksim_destroy(p->ls);

  qemu_kill(q);
  q->reader_run = 0;
  if(q->console_fd >= 0) {
    shutdown(q->console_fd, SHUT_RDWR);
    close(q->console_fd);
  }
  pthread_join(q->reader, NULL);
  if(q->console_log)
    fclose(q->console_log);
  free(q->buf);
  free(q);
  free(p);
}

peer_t *
peer_qemu_create(const peer_qemu_cfg_t *cfg, uint64_t seed)
{
  peer_t *p = calloc(1, sizeof(*p));
  qemu_priv_t *q = calloc(1, sizeof(*q));
  p->priv = q;
  q->cfg = *cfg;
  if(q->cfg.qemu_bin == NULL)
    q->cfg.qemu_bin = "qemu-system-arm";
  if(q->cfg.udp_port == 0)
    q->cfg.udp_port = DSIG_UDP_DEFAULT_PORT;
  q->console_fd = -1;
  q->console_port = 30000 + (getpid() % 20000);
  q->buf = malloc(CONSOLE_BUF_SIZE + 1);
  pthread_mutex_init(&q->mutex, NULL);
  pthread_mutex_init(&q->cmd_mutex, NULL);
  pthread_mutex_init(&q->client_mutex, NULL);
  pthread_condattr_t ca;
  pthread_condattr_init(&ca);
  pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
  pthread_cond_init(&q->cond, &ca);

  peer_init_common(p);
  p->name = cfg->mtu > 8 ? "qemu-mtu64" : "qemu-mtu8";
  p->mtu = cfg->mtu;
  p->timeout = cfg->timeout;
  p->vllp_flags = cfg->mtu > 8 ? VLLP_FDCAN_ADAPTATION : 0;
  p->has_log_service = 1;
  p->reliable_blackout = 0;
  p->server_fragment_size = 508; /* PBUF_DATA_SIZE - 4 */
  p->max_message_size = 4 * 512 - 4; /* VLLP_MAX_MESSAGE_PBUFS * PBUF_DATA_SIZE - CRC */
  p->ls = linksim_create(seed);

  mkdir(cfg->logdir, 0755);
  char path[512];
  snprintf(path, sizeof(path), "%s/console.log", cfg->logdir);
  q->console_log = fopen(path, "w");

  if(qemu_spawn(p) < 0) {
    tst_logf("qemu: spawn failed");
    goto fail;
  }
  if(console_connect(q) < 0) {
    tst_logf("qemu: could not connect to console on port %d",
             q->console_port);
    goto fail;
  }
  q->reader_run = 1;
  pthread_create(&q->reader, NULL, console_reader, p);

  /* Wait for the CLI prompt (the guest boots once we are connected,
     see wait=on above), then sync with an empty command */
  if(console_wait_for(q, 0, PROMPT, 15000000) < 0) {
    tst_logf("qemu: no CLI prompt within 15s");
    goto fail;
  }
  char out[256];
  if(console_cmd(p, "", out, sizeof(out)) < 0)
    tst_logf("qemu: console sync failed (continuing)");

  q->udp = dsig_udp_create(NULL, q->cfg.udp_port, NULL);
  if(q->udp == NULL) {
    tst_logf("qemu: dsig_udp_create failed");
    goto fail;
  }
  q->bus = dsig_create(dsig_udp_tx, q->udp);
  dsig_udp_start(q->udp, q->bus);
  q->sub = dsig_sub(q->bus, q->cfg.rxid, 0xffffffff, 0, udp_rx, p);

  p->v = make_client(p);

  p->restart_client = qemu_restart_client;
  p->server_status = qemu_server_status;
  p->server_console = console_cmd;
  p->destroy = qemu_destroy;
  return p;

fail:
  qemu_kill(q);
  if(q->console_fd >= 0)
    close(q->console_fd);
  if(q->reader_run) {
    q->reader_run = 0;
    pthread_join(q->reader, NULL);
  }
  linksim_destroy(p->ls);
  free(q->buf);
  free(q);
  free(p);
  return NULL;
}
