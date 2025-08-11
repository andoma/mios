#include "vllp.h"

#include <pthread.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <termios.h>

static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;
static vllp_channel_t *g_vc;
static const char *g_name;

static void
vllp_term_rx(void *opaque, const void *data, size_t length)
{
  if(write(1, data, length) == 1) {
    return;
  }
}

static void
vllp_term_eof(void *opaque, int error)
{
  fprintf(stderr, "* EOF, error: %d\n", error);
}


void
vllp_terminal(vllp_t *v, const char *name)
{
  g_name = name;
  pthread_mutex_lock(&g_mtx);
  g_vc = vllp_channel_create(v, g_name, 0, vllp_term_rx, vllp_term_eof, v);
  pthread_mutex_unlock(&g_mtx);

  struct termios termio;

  printf("Exit with ^B\n");

  if(!isatty(0)) {
    fprintf(stderr, "stdin is not a tty\n");
    exit(1);
  }
  if(tcgetattr(0, &termio) == -1) {
    perror("tcgetattr");
    exit(1);
  }

  struct termios termio2;
  termio2 = termio;
  termio2.c_lflag &= ~(ECHO | ICANON | ISIG);

  if(tcsetattr(0, TCSANOW, &termio2) == -1)
    return;

  while(1) {
    char c;
    if(read(0, &c, 1) != 1) {
      perror("read");
      break;
    }
    if(c == 2)
      break;
    pthread_mutex_lock(&g_mtx);
    vllp_channel_send(g_vc, &c, 1);
    pthread_mutex_unlock(&g_mtx);

  }

  fprintf(stderr, "* Sending close\n");
  tcsetattr(0, TCSANOW, &termio);

  pthread_mutex_lock(&g_mtx);
  vllp_channel_close(g_vc, 0, 1);
  pthread_mutex_unlock(&g_mtx);
  usleep(10000);
  printf("Exiting...\n");
  exit(0);
}
