#define _GNU_SOURCE
#include "dsig_udp.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <net/if.h>

struct dsig_udp {
  int fd;
  struct sockaddr_in dst;
  dsig_t *bus;
  pthread_t rx_tid;
  int rx_running;
};

dsig_udp_t *
dsig_udp_create(const char *group, uint16_t port, const char *bind_ifname)
{
  if(group == NULL)
    group = DSIG_UDP_DEFAULT_GROUP;
  if(port == 0)
    port = DSIG_UDP_DEFAULT_PORT;

  in_addr_t group_addr = inet_addr(group);
  if(group_addr == INADDR_NONE)
    return NULL;

  in_addr_t if_addr = htonl(INADDR_ANY);
  if(bind_ifname != NULL) {
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, bind_ifname, IFNAMSIZ - 1);
    int probe = socket(AF_INET, SOCK_DGRAM, 0);
    if(probe < 0)
      return NULL;
    if(ioctl(probe, SIOCGIFADDR, &ifr) < 0) {
      close(probe);
      return NULL;
    }
    if_addr = ((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr.s_addr;
    close(probe);
  }

  int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if(fd < 0)
    return NULL;

  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  struct sockaddr_in bind_addr;
  memset(&bind_addr, 0, sizeof(bind_addr));
  bind_addr.sin_family = AF_INET;
  bind_addr.sin_port = htons(port);
  bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if(bind(fd, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
    close(fd);
    return NULL;
  }

  struct ip_mreq mreq;
  mreq.imr_multiaddr.s_addr = group_addr;
  mreq.imr_interface.s_addr = if_addr;
  if(setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
    close(fd);
    return NULL;
  }

  if(if_addr != htonl(INADDR_ANY)) {
    struct in_addr ifa = { .s_addr = if_addr };
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &ifa, sizeof(ifa));
  }

  int loop = 1;
  setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));

  dsig_udp_t *t = calloc(1, sizeof(*t));
  if(t == NULL) {
    close(fd);
    return NULL;
  }
  t->fd = fd;
  t->dst.sin_family = AF_INET;
  t->dst.sin_port = htons(port);
  t->dst.sin_addr.s_addr = group_addr;
  return t;
}

void
dsig_udp_tx(void *opaque, uint32_t signal, const void *data, size_t len)
{
  dsig_udp_t *t = opaque;
  uint8_t buf[4 + 1500];
  if(len > sizeof(buf) - 4)
    return;
  buf[0] = signal;
  buf[1] = signal >> 8;
  buf[2] = signal >> 16;
  buf[3] = signal >> 24;
  if(len)
    memcpy(buf + 4, data, len);
  sendto(t->fd, buf, 4 + len, 0,
         (struct sockaddr *)&t->dst, sizeof(t->dst));
}

static void *
rx_thread(void *arg)
{
  dsig_udp_t *t = arg;
  uint8_t buf[4 + 1500];
  while(1) {
    ssize_t n = recv(t->fd, buf, sizeof(buf), 0);
    if(n < 0) {
      if(errno == EINTR)
        continue;
      break;
    }
    if(n < 4)
      continue;
    uint32_t signal = (uint32_t)buf[0]        |
                      ((uint32_t)buf[1] << 8) |
                      ((uint32_t)buf[2] << 16)|
                      ((uint32_t)buf[3] << 24);
    dsig_input(t->bus, signal, buf + 4, n - 4);
  }
  return NULL;
}

int
dsig_udp_start(dsig_udp_t *t, dsig_t *bus)
{
  t->bus = bus;
  if(pthread_create(&t->rx_tid, NULL, rx_thread, t))
    return -1;
  t->rx_running = 1;
  return 0;
}

void
dsig_udp_destroy(dsig_udp_t *t)
{
  if(t == NULL)
    return;
  if(t->rx_running) {
    shutdown(t->fd, SHUT_RDWR);
    pthread_join(t->rx_tid, NULL);
  }
  close(t->fd);
  free(t);
}
