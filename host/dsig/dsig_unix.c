#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "dsig_unix.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#define MAX_PEERS 16

struct dsig_unix {
  int fd;
  char bind_path[sizeof(((struct sockaddr_un *)0)->sun_path)];

  pthread_mutex_t peer_lock;
  struct sockaddr_un peers[MAX_PEERS];
  socklen_t peer_lens[MAX_PEERS];
  int num_peers;

  dsig_t *bus;
  pthread_t rx_tid;
  int rx_running;
  volatile int stop;
};

// Caller holds peer_lock.
static void
add_peer_locked(dsig_unix_t *t, const struct sockaddr_un *addr, socklen_t len)
{
  for(int i = 0; i < t->num_peers; i++) {
    if(t->peer_lens[i] == len && !memcmp(&t->peers[i], addr, len))
      return; // already known
  }
  if(t->num_peers >= MAX_PEERS)
    return; // best-effort: drop silently, existing peers still work
  t->peers[t->num_peers] = *addr;
  t->peer_lens[t->num_peers] = len;
  t->num_peers++;
}

dsig_unix_t *
dsig_unix_create(const char *bind_path, const char *peer_path)
{
  if(bind_path == NULL && peer_path == NULL)
    return NULL;

  int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if(fd < 0)
    return NULL;

  dsig_unix_t *t = calloc(1, sizeof(*t));
  if(t == NULL) {
    close(fd);
    return NULL;
  }
  t->fd = fd;
  pthread_mutex_init(&t->peer_lock, NULL);

  char generated[64];
  if(bind_path == NULL) {
    snprintf(generated, sizeof(generated), "/tmp/dsig-%d.sock", (int)getpid());
    bind_path = generated;
  }

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", bind_path);

  unlink(bind_path); // clear a stale socket file left by a prior instance
  if(bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    close(fd);
    pthread_mutex_destroy(&t->peer_lock);
    free(t);
    return NULL;
  }
  snprintf(t->bind_path, sizeof(t->bind_path), "%s", bind_path);

  struct timeval rcvtimeo = { 0, 200000 };
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));

  if(peer_path != NULL) {
    struct sockaddr_un paddr;
    memset(&paddr, 0, sizeof(paddr));
    paddr.sun_family = AF_UNIX;
    snprintf(paddr.sun_path, sizeof(paddr.sun_path), "%s", peer_path);
    pthread_mutex_lock(&t->peer_lock);
    add_peer_locked(t, &paddr, sizeof(paddr));
    pthread_mutex_unlock(&t->peer_lock);
  }

  return t;
}

void
dsig_unix_tx(void *opaque, uint32_t signal, const void *data, size_t len)
{
  dsig_unix_t *t = opaque;
  uint8_t buf[4 + 1500];
  if(len > sizeof(buf) - 4)
    return;
  buf[0] = signal;
  buf[1] = signal >> 8;
  buf[2] = signal >> 16;
  buf[3] = signal >> 24;
  if(len)
    memcpy(buf + 4, data, len);

  pthread_mutex_lock(&t->peer_lock);
  for(int i = 0; i < t->num_peers; i++) {
    sendto(t->fd, buf, 4 + len, 0,
           (struct sockaddr *)&t->peers[i], t->peer_lens[i]);
  }
  pthread_mutex_unlock(&t->peer_lock);
}

static void *
rx_thread(void *arg)
{
  dsig_unix_t *t = arg;
  uint8_t buf[4 + 1500];
  while(!t->stop) {
    struct sockaddr_un from;
    socklen_t fromlen = sizeof(from);
    ssize_t n = recvfrom(t->fd, buf, sizeof(buf), 0,
                        (struct sockaddr *)&from, &fromlen);
    if(n < 0) {
      if(errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
        continue; // EAGAIN/EWOULDBLOCK: SO_RCVTIMEO fired, recheck t->stop
      break;
    }
    if(n < 4)
      continue;

    // A peer with no bound address of its own (shouldn't happen for our
    // own tools, which always bind) has nothing useful to reply to.
    if(fromlen > (socklen_t)sizeof(sa_family_t)) {
      pthread_mutex_lock(&t->peer_lock);
      add_peer_locked(t, &from, fromlen);
      pthread_mutex_unlock(&t->peer_lock);
    }

    uint32_t signal = (uint32_t)buf[0]        |
                      ((uint32_t)buf[1] << 8) |
                      ((uint32_t)buf[2] << 16)|
                      ((uint32_t)buf[3] << 24);
    dsig_input(t->bus, signal, buf + 4, n - 4);
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
  close(t->fd);
  unlink(t->bind_path);
  pthread_mutex_destroy(&t->peer_lock);
  free(t);
}
