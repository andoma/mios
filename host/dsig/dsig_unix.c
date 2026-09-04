#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "dsig_unix.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

// AF_UNIX SOCK_STREAM, framed as [u32 LE frame_len][u32 LE signal][payload],
// where frame_len counts everything after itself (4 + payload length).
// SOCK_SEQPACKET would avoid needing the length prefix, but macOS never
// implemented it for AF_UNIX -- STREAM is the portable choice.

#define MAX_CONNS 16
#define MAX_PAYLOAD 1500
#define MAX_FRAME_BODY (4 + MAX_PAYLOAD)
#define RX_BUF_SIZE 4096

struct unix_conn {
  int fd;
  uint8_t buf[RX_BUF_SIZE];
  size_t len;
};

struct dsig_unix {
  int is_server;
  int listen_fd; // server only

  char bind_path[sizeof(((struct sockaddr_un *)0)->sun_path)]; // server only
  char peer_path[sizeof(((struct sockaddr_un *)0)->sun_path)]; // client only

  pthread_mutex_t conn_lock;
  struct unix_conn conns[MAX_CONNS];
  int num_conns;

  dsig_t *bus;
  pthread_t rx_tid;
  int rx_running;
  volatile int stop;
  unsigned int tx_stalls;  // peers dropped for not reading, see dsig_unix_tx()
};

static int64_t
now_ms(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

dsig_unix_t *
dsig_unix_create(const char *bind_path, const char *peer_path)
{
  if(bind_path == NULL && peer_path == NULL)
    return NULL;

  dsig_unix_t *t = calloc(1, sizeof(*t));
  if(t == NULL)
    return NULL;
  t->listen_fd = -1;
  pthread_mutex_init(&t->conn_lock, NULL);

  t->is_server = (peer_path == NULL);

  if(t->is_server) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if(fd < 0) {
      pthread_mutex_destroy(&t->conn_lock);
      free(t);
      return NULL;
    }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", bind_path);

    unlink(bind_path); // clear a stale socket file left by a prior instance
    if(bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
       listen(fd, MAX_CONNS) < 0) {
      close(fd);
      pthread_mutex_destroy(&t->conn_lock);
      free(t);
      return NULL;
    }
    snprintf(t->bind_path, sizeof(t->bind_path), "%s", bind_path);
    t->listen_fd = fd;
  } else {
    snprintf(t->peer_path, sizeof(t->peer_path), "%s", peer_path);
    // Connection is established lazily by the rx thread (retried until
    // the server -- e.g. fcmon -- is actually up), not here.
  }

  return t;
}

// Caller holds conn_lock. Swap-with-last removal: safe when walking the
// conns array from the highest index down (see rx_thread()).
static void
remove_conn_locked(dsig_unix_t *t, int idx)
{
  close(t->conns[idx].fd);
  t->conns[idx] = t->conns[t->num_conns - 1];
  t->num_conns--;
}

void
dsig_unix_tx(void *opaque, uint32_t signal, const void *data, size_t len)
{
  dsig_unix_t *t = opaque;
  if(len > MAX_PAYLOAD)
    return;

  uint8_t frame[4 + MAX_FRAME_BODY];
  uint32_t body_len = 4 + (uint32_t)len;
  frame[0] = body_len;
  frame[1] = body_len >> 8;
  frame[2] = body_len >> 16;
  frame[3] = body_len >> 24;
  frame[4] = signal;
  frame[5] = signal >> 8;
  frame[6] = signal >> 16;
  frame[7] = signal >> 24;
  if(len)
    memcpy(frame + 8, data, len);
  size_t total = 4 + body_len;

  // Only the rx thread ever removes/closes a connection (it's the one
  // that observes EOF/errors via poll()); a write() failure here just
  // means that peer is already dead and about to be reaped there, so
  // we don't touch the array, just skip it.
  //
  // Never block. A peer that has stopped reading (a stuck GUI, a tool
  // that only publishes, a relay wedged on *its* peer) fills its socket
  // buffer in seconds, and a blocking send() here would then stall the
  // caller -- while holding conn_lock, which also stalls the rx thread,
  // which stalls whoever is sending to us: that is how two peers relaying
  // to each other once froze together. Framing is a byte stream, so a frame cannot
  // be half-sent and dropped; instead the slow peer is shut down and the
  // rx thread reaps it on the next poll(). dsig is lossy pub/sub, a peer
  // that far behind has already lost.
  pthread_mutex_lock(&t->conn_lock);
  for(int i = 0; i < t->num_conns; i++) {
    size_t off = 0;
    while(off < total) {
      ssize_t n = send(t->conns[i].fd, frame + off, total - off,
                       MSG_NOSIGNAL | MSG_DONTWAIT);
      if(n < 0) {
        if(errno == EINTR)
          continue;
        if(errno == EAGAIN || errno == EWOULDBLOCK) {
          t->tx_stalls++;
          shutdown(t->conns[i].fd, SHUT_RDWR);
        }
        break;
      }
      off += (size_t)n;
    }
  }
  pthread_mutex_unlock(&t->conn_lock);
}

unsigned int
dsig_unix_tx_stalls(const dsig_unix_t *t)
{
  return t->tx_stalls;
}

// rx thread only (the sole writer of conn buffers). Drains as many
// complete frames as are buffered.
static void
process_conn_data(dsig_unix_t *t, struct unix_conn *c)
{
  for(;;) {
    if(c->len < 4)
      return;
    uint32_t body_len = (uint32_t)c->buf[0] | ((uint32_t)c->buf[1] << 8) |
      ((uint32_t)c->buf[2] << 16) | ((uint32_t)c->buf[3] << 24);
    if(body_len < 4 || body_len > MAX_FRAME_BODY) {
      c->len = 0; // desynced stream, nothing sane to recover -- drop it all
      return;
    }
    if(c->len < 4 + body_len)
      return; // wait for the rest of this frame
    uint32_t signal = (uint32_t)c->buf[4] | ((uint32_t)c->buf[5] << 8) |
      ((uint32_t)c->buf[6] << 16) | ((uint32_t)c->buf[7] << 24);
    dsig_input(t->bus, signal, c->buf + 8, body_len - 4);
    size_t consumed = 4 + body_len;
    memmove(c->buf, c->buf + consumed, c->len - consumed);
    c->len -= consumed;
  }
}

static void
try_connect(dsig_unix_t *t)
{
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if(fd < 0)
    return;
  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", t->peer_path);
  if(connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    close(fd);
    return;
  }
  pthread_mutex_lock(&t->conn_lock);
  t->conns[0].fd = fd;
  t->conns[0].len = 0;
  t->num_conns = 1;
  pthread_mutex_unlock(&t->conn_lock);
}

static void *
rx_thread(void *arg)
{
  dsig_unix_t *t = arg;
  int64_t next_connect_attempt = 0;

  while(!t->stop) {
    if(!t->is_server && t->num_conns == 0) {
      int64_t now = now_ms();
      if(now >= next_connect_attempt) {
        try_connect(t);
        next_connect_attempt = now + 500;
      }
      if(t->num_conns == 0) {
        usleep(100000);
        continue;
      }
    }

    struct pollfd pfds[1 + MAX_CONNS];
    int nfds = 0;
    int listen_idx = -1;
    int conn_base;

    pthread_mutex_lock(&t->conn_lock);
    const int nconns_before = t->num_conns;
    if(t->is_server) {
      pfds[nfds].fd = t->listen_fd;
      pfds[nfds].events = POLLIN;
      listen_idx = nfds++;
    }
    conn_base = nfds;
    for(int i = 0; i < t->num_conns; i++) {
      pfds[nfds].fd = t->conns[i].fd;
      pfds[nfds].events = POLLIN;
      nfds++;
    }
    pthread_mutex_unlock(&t->conn_lock);

    int r = poll(pfds, nfds, 200);
    if(r < 0) {
      if(errno == EINTR)
        continue;
      break;
    }
    if(r == 0)
      continue;

    if(listen_idx >= 0 && (pfds[listen_idx].revents & POLLIN)) {
      int newfd = accept(t->listen_fd, NULL, NULL);
      if(newfd >= 0) {
        pthread_mutex_lock(&t->conn_lock);
        if(t->num_conns < MAX_CONNS) {
          t->conns[t->num_conns].fd = newfd;
          t->conns[t->num_conns].len = 0;
          t->num_conns++;
        } else {
          close(newfd); // best-effort cap, same spirit as the old MAX_PEERS
        }
        pthread_mutex_unlock(&t->conn_lock);
      }
    }

    // Read under conn_lock (the array may be reshaped here), but dispatch
    // frames to the bus *without* it: dsig_input() takes the bus lock,
    // and the bus holds that lock while calling dsig_unix_tx(), which
    // takes conn_lock -- dispatching under conn_lock is a lock-order
    // inversion that deadlocks the moment a frame arrives while an
    // emitter fires. Safe without the lock because only this thread ever
    // modifies the conns array or a conn's buffer; tx only reads fds.
    int dispatch[MAX_CONNS];
    int ndispatch = 0;
    pthread_mutex_lock(&t->conn_lock);
    // Walk downward: remove_conn_locked() only ever swaps in an element
    // at or below the current index, so already-visited (higher) slots
    // are never disturbed.
    for(int i = t->num_conns - 1; i >= 0; i--) {
      int pi = conn_base + i;
      if(pi >= nfds || !(pfds[pi].revents & (POLLIN | POLLERR | POLLHUP)))
        continue;
      struct unix_conn *c = &t->conns[i];
      if(c->len == sizeof(c->buf)) {
        remove_conn_locked(t, i); // no complete frame ever drained: desynced
        continue;
      }
      ssize_t n = read(c->fd, c->buf + c->len, sizeof(c->buf) - c->len);
      if(n <= 0) {
        remove_conn_locked(t, i); // EOF or error: a real disconnect
        continue;
      }
      c->len += (size_t)n;
      dispatch[ndispatch++] = i;
    }
    pthread_mutex_unlock(&t->conn_lock);

    // A removal above swapped the last element into a lower slot, which
    // may itself be in dispatch[] under its old index; both indexes then
    // point at valid conns (the walk is downward, removals only move
    // elements down), and a frame parsed twice is harmless only if it is
    // not -- so simply skip dispatch when anything was removed this round
    // and let the next poll() deliver it.
    if(ndispatch > 0 && t->num_conns == nconns_before) {
      for(int k = 0; k < ndispatch; k++)
        process_conn_data(t, &t->conns[dispatch[k]]);
    }
  }
  return NULL;
}

int
dsig_unix_start(dsig_unix_t *t, dsig_t *bus)
{
  t->bus = bus;
  if(pthread_create(&t->rx_tid, NULL, rx_thread, t))
    return -1;
  t->rx_running = 1;
  return 0;
}

void
dsig_unix_destroy(dsig_unix_t *t)
{
  if(t == NULL)
    return;
  if(t->rx_running) {
    t->stop = 1;
    pthread_join(t->rx_tid, NULL);
  }
  for(int i = 0; i < t->num_conns; i++)
    close(t->conns[i].fd);
  if(t->is_server) {
    close(t->listen_fd);
    unlink(t->bind_path);
  }
  pthread_mutex_destroy(&t->conn_lock);
  free(t);
}
