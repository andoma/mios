#define _GNU_SOURCE
#include "dsig_cansock.h"

#include <errno.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

struct dsig_cansock {
  int fd;
  dsig_t *bus;
  pthread_t rx_tid;
  int rx_running;
};

dsig_cansock_t *
dsig_cansock_create(const char *ifname)
{
  if(ifname == NULL)
    ifname = getenv("IFC");
  if(ifname == NULL)
    ifname = "can0";

  int fd = socket(PF_CAN, SOCK_RAW | SOCK_CLOEXEC, CAN_RAW);
  if(fd < 0)
    return NULL;

  unsigned int ifindex = if_nametoindex(ifname);
  if(ifindex == 0) {
    close(fd);
    return NULL;
  }

  int loop = 0;
  setsockopt(fd, SOL_CAN_RAW, CAN_RAW_LOOPBACK, &loop, sizeof(loop));
  int en = 1;
  setsockopt(fd, SOL_CAN_RAW, CAN_RAW_FD_FRAMES, &en, sizeof(en));

  struct sockaddr_can addr;
  memset(&addr, 0, sizeof(addr));
  addr.can_family = AF_CAN;
  addr.can_ifindex = ifindex;
  if(bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    close(fd);
    return NULL;
  }

  dsig_cansock_t *t = calloc(1, sizeof(*t));
  if(t == NULL) {
    close(fd);
    return NULL;
  }
  t->fd = fd;
  return t;
}

void
dsig_cansock_tx(void *opaque, uint32_t signal, const void *data, size_t len)
{
  dsig_cansock_t *t = opaque;
  struct canfd_frame frame;
  memset(&frame, 0, sizeof(frame));
  frame.can_id = signal;
  if(len > sizeof(frame.data))
    len = sizeof(frame.data);
  frame.len = len;
  if(len)
    memcpy(frame.data, data, len);
  ssize_t r = write(t->fd, &frame, sizeof(frame));
  (void)r;
}

static void *
rx_thread(void *arg)
{
  dsig_cansock_t *t = arg;
  struct canfd_frame frame;
  while(1) {
    ssize_t n = read(t->fd, &frame, sizeof(frame));
    if(n < 0) {
      if(errno == EINTR)
        continue;
      break;
    }
    if(n < (ssize_t)sizeof(struct can_frame))
      continue;
    dsig_input(t->bus, frame.can_id & CAN_EFF_MASK, frame.data, frame.len);
  }
  return NULL;
}

int
dsig_cansock_start(dsig_cansock_t *t, dsig_t *bus)
{
  t->bus = bus;
  if(pthread_create(&t->rx_tid, NULL, rx_thread, t))
    return -1;
  t->rx_running = 1;
  return 0;
}

void
dsig_cansock_destroy(dsig_cansock_t *t)
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
