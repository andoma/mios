#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "vllp_telnetd.h"

#include "vllp.h"

#include <pthread.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/queue.h>
#include <poll.h>
#include <fcntl.h>
#include <errno.h>

#ifndef ECOMM
#define ECOMM EIO // macOS has no ECOMM
#endif

typedef struct vllp_telnetd_sock {
  LIST_ENTRY(vllp_telnetd_sock) link;
  vllp_channel_t *vc;
  int fd;
  uint8_t iac_state;
} vllp_telnetd_sock_t;

struct vllp_telnetd {
  vllp_t *v;
  char *service;
  LIST_HEAD(, vllp_telnetd_sock) sockets;
  pthread_t tid;
};

/* Upper bound on simultaneously open telnet connections (plus the
 * listening socket). Connections beyond this are still accepted but
 * won't be polled until another one closes. */
#define VTD_MAX_POLLFDS 32


static int
safewrite(int fd, const void *data, size_t len)
{
  while(len) {

    int n = write(fd, data, len);
    if(n < 1) {
      if(n == 0)
        errno = ECOMM;
      return -1;
    }
    data += n;
    len -= n;
  }
  return 0;
}


static vllp_telnetd_sock_t *
vtd_add_fd(vllp_telnetd_t *vtd, int fd)
{
  vllp_telnetd_sock_t *vts = calloc(1, sizeof(vllp_telnetd_sock_t));
  vts->fd = fd;
  LIST_INSERT_HEAD(&vtd->sockets, vts, link);
  return vts;
}


static void
vtd_rx(void *opaque, const void *data, size_t length)
{
  vllp_telnetd_sock_t *vts = opaque;

  const uint8_t *u8 = data;
  uint8_t fix[length * 2];
  size_t outlen = 0;
  for(size_t i = 0; i < length; i++) {
    if(u8[i] == 10) {
      fix[outlen++] = 13;
    }
    fix[outlen++] = u8[i];
  }

  safewrite(vts->fd, fix, outlen);
}

static void
vtd_eof(void *opaque, int error)
{
  vllp_telnetd_sock_t *vts = opaque;
  shutdown(vts->fd, 2);
}


static void
vts_close(vllp_telnetd_t *vtd, vllp_telnetd_sock_t *vts)
{
  vllp_channel_close(vts->vc, 0, 0);

  LIST_REMOVE(vts, link);
  close(vts->fd);
  free(vts);
}


static const uint8_t telnet_init[] = {
  255, 251, 1, // WILL ECHO
  255, 251, 0, // WILL BINARY
  255, 251, 3, // WILL SUPRESS-GO-AHEAD
};


static int
telnet_intercept(uint8_t *buf, int r, uint8_t *state)
{
  size_t wrptr = 0;
  for(int i = 0; i < r; i++) {
    uint8_t c = buf[i];
    switch(*state) {
    case 0:
      if(c == 0) {
        continue;
      }

      if(c == 255) {
        *state = c;
      } else {
        buf[wrptr++] = c;
      }
      break;
    case 255:
      switch(c) {
      case 255:
        buf[wrptr++] = c;
        break;
      case 240 ... 250:
        c = 0;
        // FALLTHRU
      default:
        *state = c;
        break;
      }
      break;
    default:
      *state = 0;
      break;
    }
  }
  return wrptr;
}


static void *
vtd_thread(void *arg)
{
  vllp_telnetd_t *vtd = arg;

  pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

  while(1) {
    struct sockaddr_in remote;
    socklen_t slen = sizeof(remote);

    struct pollfd pfds[VTD_MAX_POLLFDS];
    vllp_telnetd_sock_t *socks[VTD_MAX_POLLFDS];
    int nfds = 0;
    vllp_telnetd_sock_t *vts;
    LIST_FOREACH(vts, &vtd->sockets, link) {
      if(nfds == VTD_MAX_POLLFDS)
        break;
      pfds[nfds].fd = vts->fd;
      pfds[nfds].events = POLLIN;
      pfds[nfds].revents = 0;
      socks[nfds] = vts;
      nfds++;
    }

    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    int n = poll(pfds, nfds, -1);
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
    if(n == -1) {
      if(errno == EINTR)
        continue;
      perror("poll");
      sleep(1);
      continue;
    }

    // Handle only the first ready fd per iteration; vts_close() mutates
    // the socket list, so we rebuild the pollfd set before continuing.
    int i;
    for(i = 0; i < nfds; i++)
      if(pfds[i].revents)
        break;

    if(i < nfds) {
      vts = socks[i];
      if(vts->vc == NULL) {

#ifdef __linux__
        int fd = accept4(vts->fd, (struct sockaddr *)&remote, &slen, SOCK_CLOEXEC);
#else
        int fd = accept(vts->fd, (struct sockaddr *)&remote, &slen);
        if(fd != -1)
          fcntl(fd, F_SETFD, FD_CLOEXEC);
#endif
        if(fd == -1) {
          perror("accept(vllp telnetd socket)");
          sleep(1);
          continue;
        }

        safewrite(fd, telnet_init, sizeof(telnet_init));

        vts = vtd_add_fd(vtd, fd);
        vts->vc = vllp_channel_create(vtd->v, vtd->service, 0, vtd_rx,
                                      vtd_eof, NULL, vts);

      } else {

        uint8_t buf[64];

        int rd = read(vts->fd, buf, sizeof(buf));
        if(rd > 0) {

          rd = telnet_intercept(buf, rd, &vts->iac_state);

          if(rd > 0)
            vllp_channel_send(vts->vc, buf, rd);

        } else if(rd == 0) {
          vts_close(vtd, vts);
        } else {
          switch(errno) {
          case EAGAIN:
            break;
          default:
            vts_close(vtd, vts);
            break;
          }
        }
      }
    }
  }
  return NULL;
}


vllp_telnetd_t *
vllp_telnetd_create(struct vllp *v, const char *service, int port)
{
#ifdef SOCK_CLOEXEC
  int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
#else
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if(fd != -1)
    fcntl(fd, F_SETFD, FD_CLOEXEC);
#endif
  if(fd == -1) {
    perror("socket");
    return NULL;
  }

  const int one = 1;
  if(setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(int))) {
    perror("SO_REUSEADDR");
  }

  struct sockaddr_in sin = {
    .sin_family = AF_INET,
    .sin_port = htons(port),
    .sin_addr.s_addr = htonl(INADDR_LOOPBACK)
  };

  if(bind(fd, (struct sockaddr *)&sin, sizeof(sin))) {
    perror("bind");
    close(fd);
    return NULL;
  }

  if(listen(fd, 1)) {
    perror("listen");
  }

  vllp_telnetd_t *vtd = calloc(1, sizeof(vllp_telnetd_t));
  vtd->v = v;
  vtd->service = strdup(service);

  vtd_add_fd(vtd, fd);

  pthread_create(&vtd->tid, NULL, vtd_thread, vtd);
  return vtd;
}



void
vllp_telnetd_destroy(struct vllp_telnetd *vtd)
{
  pthread_cancel(vtd->tid);
  pthread_join(vtd->tid, NULL);
  free(vtd->service);
  vllp_telnetd_sock_t *vts;
  while((vts = LIST_FIRST(&vtd->sockets)) != NULL) {
    LIST_REMOVE(vts, link);
    close(vts->fd);

    if(vts->vc != NULL) {
      vllp_channel_close(vts->vc, 0, 0);
    }

    free(vts);

  }

  free(vtd);
}
