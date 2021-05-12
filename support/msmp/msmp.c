#include <pthread.h>
#include <errno.h>
#include <stdint.h>
#include <locale.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <poll.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <mios/stream.h>

#include "hdlc.h"

static struct termios tio;
static int fd;
static int g_metrics_fd = -1;

static void __attribute__((unused))
hexdump(const char *pfx, const void *data_, int len)
{
  int i, j, k;
  const uint8_t *data = data_;
  char buf[100];

  for(i = 0; i < len; i+= 16) {
    int p = snprintf(buf, sizeof(buf), "0x%06x: ", i);

    for(j = 0; j + i < len && j < 16; j++) {
      p += snprintf(buf + p, sizeof(buf) - p, "%s%02x ",
                    j==8 ? " " : "", data[i+j]);
    }
    const int cnt = (17 - j) * 3 + (j < 8);
    for(k = 0; k < cnt; k++)
      buf[p + k] = ' ';
    p += cnt;

    for(j = 0; j + i < len && j < 16; j++)
      buf[p++] = data[i+j] < 32 || data[i+j] > 126 ? '.' : data[i+j];
    buf[p] = 0;
    fprintf(stderr, "%s: %s\n", pfx, buf);
  }
}



static int
msmp_read(struct stream *s, void *buf, size_t size, int wait)
{
  int r = read(fd, buf, size);
  if(r == -1 && errno == EAGAIN)
    return 0;
  if(r == -1) {
    perror("read");
    exit(1);
  }
  //hexdump("INPUT", buf, r);
  return r;
}

static void
msmp_write(struct stream *s, const void *buf, size_t size)
{
  if(write(fd, buf, size)) {}
}


static stream_t g_stream = {
  .read = msmp_read,
  .write = msmp_write,
};


static void
console_send(const uint8_t *buf, size_t len)
{
  uint8_t hdr[3] = {0};
  struct iovec iov[2] = {
    {
     .iov_base = hdr,
     .iov_len = sizeof(hdr)
    }, {
     .iov_base = (void *)buf,
     .iov_len = len
    }
  };

  hdlc_sendv(&g_stream, iov, 2);
}





static void
handle_input_packet(const uint8_t *data, size_t len)
{
  switch(data[0]) {
  case 0:
    if(write(0, data + 1, len - 1)) {}
    break;
  case 1:
    if(g_metrics_fd != -1) {
      if(write(g_metrics_fd, data + 1, len - 1)) {}
    }
    break;

  }
}




static void *
read_thread(void *arg)
{
  uint8_t buf[512];
  while(1) {
    int r = hdlc_read_to_buf(&g_stream, buf, sizeof(buf),
                             STREAM_READ_WAIT_ALL);
    if(r > 1) {
      handle_input_packet(buf, r);
    }
  }
  return NULL;
}


/**
 *
 */
int
setupdev(int baudrate)
{
  int cflags = CS8 | CLOCAL | CREAD;

  switch(baudrate) {
  case 2400:
    cflags |= B2400;
    break;
  case 9600:
    cflags |= B9600;
    break;
  case 19200:
    cflags |= B19200;
    break;
  case 38400:
    cflags |= B38400;
    break;
  case 57600:
    cflags |= B57600;
    break;
  case 115200:
    cflags |= B115200;
    break;
  case 230400:
    cflags |= B230400;
    break;
  case 460800:
    cflags |= B460800;
    break;
  case 500000:
    cflags |= B500000;
    break;
  case 576000:
    cflags |= B576000;
    break;
  case 921600:
    cflags |= B921600;
    break;
  case 1000000:
    cflags |= B1000000;
    break;
  case 1152000:
    cflags |= B1152000;
    break;
  default:
    printf("Baudrate %d not supported\n", baudrate);
    return -1;
  }


  tio.c_cflag = cflags;
  tio.c_iflag = IGNPAR;
  cfmakeraw(&tio);

  if(tcsetattr(fd, TCSANOW, &tio)) {
    perror("tcsetattr");
    return -1;
  }
  return 0;
}

static struct termios termio;




/**
 *
 */
static void
terminal(void)
{
  struct termios termio2;
  uint8_t buf[64];

  printf("Exit with ^B\n");

  if(!isatty(0)) {
    fprintf(stderr, "stdin is not a tty\n");
    exit(1);
  }
  if(tcgetattr(0, &termio) == -1) {
    perror("tcgetattr");
    exit(1);
  }
  termio2 = termio;
  termio2.c_lflag &= ~(ECHO | ICANON | ISIG);
  if(1) {
    if(tcsetattr(0, TCSANOW, &termio2) == -1)
      return;
  }

  while(1) {
    if(read(0, buf, 1) != 1) {
      perror("read");
      break;
    }
    if(buf[0] == 2)
      break;

    console_send(buf, 1);
  }

  tcsetattr(0, TCSANOW, &termio);
  printf("Exiting...\n");
  exit(0);
}

static void
set_metrics(const char *str)
{
  int port = atoi(str);

  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if(fd == -1) {
    perror("socket");
    exit(1);
  }

  struct sockaddr_in sin = {AF_INET};
  sin.sin_port = htons(port);
  sin.sin_addr.s_addr = inet_addr("127.0.0.1");

  if(connect(fd, (struct sockaddr *)&sin, sizeof(sin)) == -1) {
    perror("connect");
    exit(1);
  }

  g_metrics_fd = fd;
}



static void
usage(const char *argv0)
{
  printf("Usage: %s OPTIONS\n\n", argv0);
  printf("   -d DEVICE    [/dev/ttyUSB0]\n");
  printf("   -b BAUDRATE  [115200]\n");
  printf("\n");
}

int
main(int argc, char **argv)
{
  const char *device = "/dev/ttyUSB0";
  int baudrate = 115200;
  int opt;

  while((opt = getopt(argc, argv, "b:d:m:h")) != -1) {
    switch(opt) {
    case 'b':
      baudrate = atoi(optarg);
      break;
    case 'd':
      device = optarg;
      break;
    case 'm':
      set_metrics(optarg);
      break;
    case 'h':
      usage(argv[0]);
      exit(0);
    default:
      usage(argv[0]);
      exit(1);
    }
  }


  fd = open(device, O_RDWR | O_NOCTTY);
  if(fd == -1) {
    perror("open serial port");
    exit(1);
  }

   setupdev(baudrate);

   pthread_t tid;
   pthread_create(&tid, NULL, read_thread, NULL);

   terminal();
   return 0;
}


