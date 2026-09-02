/*
 * Ethernet over passt (https://passt.top).
 *
 * passt is an unprivileged user-mode network stack: it terminates our
 * Ethernet frames and re-originates the traffic as ordinary sockets on
 * the host, hands out an address over DHCP, and answers ARP/NDP. No
 * TAP device, no namespaces, no capabilities.
 *
 * Wire format on the UNIX stream socket is the same as qemu's socket
 * netdev: each frame prefixed by its length as a 32-bit big-endian
 * integer. So this driver also talks to qemu instances and to any
 * future virtual switch that speaks the same framing.
 *
 * Two ways to get the socket:
 *
 *   --passt=PATH   connect to a running "passt -s PATH -m 1500"
 *   (default)      spawn passt ourselves on a socketpair, using
 *                  --fd. passt exits when we do (one-off mode).
 *                  Arguments after "--" are appended to passt's
 *                  command line, e.g.  mios -- -t 2323:23
 *   --no-net       don't
 *
 * Not used when running a test suite (see hosttest.h).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/param.h>
#include <sys/uio.h>

#include <mios/mios.h>
#include <mios/task.h>

#include <net/pbuf.h>
#include <net/ether.h>

#include "irq.h"
#include "cpu.h"
#include "linux.h"
#include "hostnet.h"

#define SYS_getrandom 318

// passt's own maximum. We advertise 1500 via -m but must survive more.
#define MAX_FRAME 65536

typedef struct passt_eth {
  ether_netif_t pe_eni;

  int pe_fd;
  int pe_irq;

  size_t pe_rxlen;                // Bytes valid in pe_rxbuf
  uint8_t pe_rxbuf[4 + MAX_FRAME];

  uint32_t pe_rx_oversize;
  uint32_t pe_rx_frames;
  uint32_t pe_disconnected;
} passt_eth_t;


static void
passt_print_info(struct device *dev, struct stream *st)
{
  passt_eth_t *pe = (passt_eth_t *)dev;
  ether_print(&pe->pe_eni, st);
  stprintf(st, "Socket fd: %d  oversize drops: %u  disconnected: %u\n",
           pe->pe_fd, pe->pe_rx_oversize, pe->pe_disconnected);
}

static const ethmac_device_class_t passt_device_class = {
  .dc = {
    .dc_class_name = "passt",
    .dc_print_info = passt_print_info,
  }
};


static error_t
passt_output(struct ether_netif *eni, pbuf_t *pkt,
             pbuf_tx_cb_t *txcb, uint32_t id)
{
  passt_eth_t *pe = (passt_eth_t *)eni;

  if(pe->pe_fd < 0) {
    eni->eni_stats.tx_qdrop++;
    pbuf_free(pkt);
    return ERR_NOT_CONNECTED;
  }

  eni->eni_stats.tx_pkt++;
  eni->eni_stats.tx_byte += pkt->pb_pktlen;

  // Length prefix + one iovec per pbuf segment
  struct iovec iov[16];
  uint8_t hdr[4] = {
    pkt->pb_pktlen >> 24, pkt->pb_pktlen >> 16,
    pkt->pb_pktlen >> 8, pkt->pb_pktlen
  };
  iov[0].iov_base = hdr;
  iov[0].iov_len = 4;
  size_t iovcnt = 1;
  size_t total = 4;

  for(pbuf_t *pb = pkt; pb != NULL; pb = pb->pb_next) {
    if(iovcnt == 16) {
      eni->eni_stats.tx_qdrop++;
      pbuf_free(pkt);
      return ERR_MTU_EXCEEDED;
    }
    iov[iovcnt].iov_base = pbuf_data(pb, 0);
    iov[iovcnt].iov_len = pb->pb_buflen;
    iovcnt++;
    total += pb->pb_buflen;
  }

  // Stream socket: a frame must go out whole, so finish partial writes.
  // The socket is blocking, partial results only happen on signals.
  struct iovec *cur = iov;
  size_t done = 0;
  while(done < total) {
    long n = linux_syscall(SYS_writev, pe->pe_fd, cur, iovcnt);
    if(n == -LINUX_EINTR)
      continue;
    if(n < 0)
      break;
    done += n;
    while(iovcnt && n >= (long)cur[0].iov_len) {
      n -= cur[0].iov_len;
      cur++;
      iovcnt--;
    }
    if(iovcnt) {
      cur[0].iov_base += n;
      cur[0].iov_len -= n;
    }
  }
  pbuf_free(pkt);
  return 0;
}


static void
passt_disconnect(passt_eth_t *pe, const char *why)
{
  printf("passt: %s, link down\n", why);
  pe->pe_disconnected++;
  linux_syscall(SYS_close, pe->pe_fd);
  pe->pe_fd = -1;
  net_task_raise(&pe->pe_eni.eni_ni.ni_task, NETIF_TASK_STATUS_DOWN);
}


static void
passt_irq(void *arg)
{
  passt_eth_t *pe = arg;
  const struct linux_timespec zero = {};
  int wakeup = 0;

  while(pe->pe_fd >= 0) {

    if(linux_poll1(pe->pe_fd, POLLIN, &zero) <= 0)
      break;

    long n = linux_syscall(SYS_read, pe->pe_fd, pe->pe_rxbuf + pe->pe_rxlen,
                           sizeof(pe->pe_rxbuf) - pe->pe_rxlen);
    if(n == -LINUX_EINTR)
      continue;
    if(n == 0) {
      passt_disconnect(pe, "connection closed");
      break;
    }
    if(n < 0) {
      passt_disconnect(pe, "read error");
      break;
    }
    pe->pe_rxlen += n;

    // Deliver every complete frame in the buffer
    size_t pos = 0;
    while(pe->pe_rxlen - pos >= 4) {
      const uint8_t *p = pe->pe_rxbuf + pos;
      const size_t flen = (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
      if(flen > MAX_FRAME) {
        passt_disconnect(pe, "bad frame length");
        pe->pe_rxlen = 0;
        pos = 0;
        break;
      }
      if(pe->pe_rxlen - pos - 4 < flen)
        break; // Incomplete, wait for more

      if(flen > 14 + 1500 + 4) {
        pe->pe_rx_oversize++;
      } else {
        host_ether_rx(&pe->pe_eni, p + 4, flen);
        pe->pe_rx_frames++;
        wakeup = 1;
      }
      pos += 4 + flen;
    }
    if(pos) {
      memmove(pe->pe_rxbuf, pe->pe_rxbuf + pos, pe->pe_rxlen - pos);
      pe->pe_rxlen -= pos;
    }
  }

  if(wakeup)
    netif_wakeup(&pe->pe_eni.eni_ni);
}


// ---- Getting a socket ----

static int
passt_connect(const char *path)
{
  struct linux_sockaddr_un sa = { .sun_family = AF_UNIX };
  if(strlen(path) >= sizeof(sa.sun_path)) {
    printf("passt: socket path too long\n");
    return -1;
  }
  strcpy(sa.sun_path, path);

  int fd = linux_syscall(SYS_socket, AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if(fd < 0)
    return -1;
  if(linux_syscall(SYS_connect, fd, &sa, sizeof(sa)) < 0) {
    printf("passt: connect to %s failed\n", path);
    linux_syscall(SYS_close, fd);
    return -1;
  }
  return fd;
}


// Locate the passt binary: $MIOS_PASST, then each directory in $PATH
static const char *
passt_find_binary(char *buf, size_t buflen)
{
  const char *p = host_env("MIOS_PASST");
  if(p != NULL)
    return p;

  const char *path = host_env("PATH");
  if(path == NULL)
    path = "/usr/local/bin:/usr/bin:/bin";

  while(*path) {
    const char *end = strchr(path, ':');
    const size_t dirlen = end ? (size_t)(end - path) : strlen(path);
    if(dirlen + 7 < buflen) {
      memcpy(buf, path, dirlen);
      strcpy(buf + dirlen, "/passt");
      // access(2) is not in our syscall shim, try opening it
      int fd = linux_syscall(SYS_open, buf, 0 /* O_RDONLY */, 0);
      if(fd >= 0) {
        linux_syscall(SYS_close, fd);
        return buf;
      }
    }
    if(end == NULL)
      break;
    path = end + 1;
  }
  return NULL;
}


static int
passt_spawn(void)
{
  char pathbuf[256];
  const char *bin = passt_find_binary(pathbuf, sizeof(pathbuf));
  if(bin == NULL) {
    printf("passt: binary not found in PATH, no network "
           "(install passt, or set MIOS_PASST, or run with --no-net)\n");
    return -1;
  }

  int sv[2];
  if(linux_syscall(SYS_socketpair, AF_UNIX, SOCK_STREAM, 0, sv) < 0)
    return -1;

  // passt -f -q --fd 3 -m 1500 [user args after "--"]
  char **tail = host_argv_tail();
  int ntail = 0;
  while(tail[ntail] != NULL)
    ntail++;

  const char *argv[16 + ntail];
  int argc = 0;
  argv[argc++] = "passt";
  argv[argc++] = "-f";
  argv[argc++] = "-q";
  argv[argc++] = "--fd";
  argv[argc++] = "3";
  argv[argc++] = "-m";
  argv[argc++] = "1500";
  for(int i = 0; i < ntail; i++)
    argv[argc++] = tail[i];
  argv[argc] = NULL;

  long pid = linux_syscall(SYS_fork);
  if(pid < 0) {
    linux_syscall(SYS_close, sv[0]);
    linux_syscall(SYS_close, sv[1]);
    return -1;
  }

  if(pid == 0) {
    // Child
    linux_syscall(SYS_close, sv[0]);
    linux_syscall(SYS_dup2, sv[1], 3);
    linux_syscall(SYS_close, sv[1]);
    linux_syscall(SYS_execve, bin, argv, host_envp());
    const char msg[] = "passt: exec failed\n";
    linux_syscall(SYS_write, 2, msg, sizeof(msg) - 1);
    linux_exit_group(127);
  }

  linux_syscall(SYS_close, sv[1]);
  printf("passt: spawned %s (pid %ld)\n", bin, pid);
  return sv[0];
}


static void  __attribute__((constructor(400)))
passt_init(void)
{
  // Test suites bring their own (virtual) network
  if(host_arg("no-net") != NULL || host_arg("list") != NULL || host_test_mode())
    return;

  int fd;
  const char *path = host_arg("passt");
  if(path != NULL && path[0]) {
    fd = passt_connect(path);
  } else {
    fd = passt_spawn();
  }
  if(fd < 0)
    return;

  passt_eth_t *pe = calloc(1, sizeof(passt_eth_t));
  pe->pe_fd = fd;

  // Locally administered, random
  uint8_t *mac = pe->pe_eni.eni_addr;
  linux_syscall(SYS_getrandom, mac, 6, 0);
  mac[0] = 0x02;

  pe->pe_eni.eni_output = passt_output;

  ether_netif_init(&pe->pe_eni, "eth0", &passt_device_class);
  ether_netif_attach(&pe->pe_eni);

  pe->pe_irq = host_irq_alloc(IRQ_LEVEL_NET, passt_irq, pe);
  host_irq_attach_fd(pe->pe_irq, fd);
  host_irq_raise(pe->pe_irq); // In case something arrived before O_ASYNC

  net_task_raise(&pe->pe_eni.eni_ni.ni_task, NETIF_TASK_STATUS_UP);
}
