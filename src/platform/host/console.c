/*
 * Console on stdin/stdout.
 *
 * Input works like a UART with an RX FIFO: the kernel raises our IRQ
 * (SIGIO via O_ASYNC) when stdin becomes readable, the handler drains
 * what is there into the FIFO and wakes the reader. If stdin is a tty
 * it is put in raw mode for the lifetime of the process; Ctrl-C then
 * terminates the process instead of reaching the shell.
 *
 * EOF on stdin exits the process. This makes "echo cmd | mios" a
 * complete test run: the shell only asks for more input once the
 * previous command has finished.
 */

#include <stdio.h>
#include <string.h>
#include <mios/task.h>
#include <mios/stream.h>
#include <mios/error.h>

#include "irq.h"
#include "cpu.h"
#include "linux.h"

#define STDIN_FD  0
#define STDOUT_FD 1

static task_waitable_t console_rx;

static uint8_t rx_fifo_rdptr;
static uint8_t rx_fifo_wrptr;

#define RX_FIFO_SIZE 128
static uint8_t rx_fifo[RX_FIFO_SIZE];

static int console_irq;
static int console_is_tty;
static int console_eof;
static int console_rx_more;   // Data left in the fd because FIFO was full
static struct linux_termios console_saved_termios;


static void
console_irq_handler(void *arg)
{
  const struct linux_timespec zero = {};

  while(1) {
    const size_t used = (uint8_t)(rx_fifo_wrptr - rx_fifo_rdptr);
    const size_t avail = RX_FIFO_SIZE - used;
    if(avail == 0) {
      console_rx_more = 1;
      break;
    }

    if(linux_poll1(STDIN_FD, POLLIN, &zero) <= 0)
      break;

    const size_t wr = rx_fifo_wrptr & (RX_FIFO_SIZE - 1);
    size_t chunk = RX_FIFO_SIZE - wr;
    if(chunk > avail)
      chunk = avail;

    long n = linux_syscall(SYS_read, STDIN_FD, rx_fifo + wr, chunk);
    if(n == -LINUX_EINTR)
      continue;
    if(n == 0) {
      console_eof = 1;
      break;
    }
    if(n < 0)
      break;

    if(console_is_tty) {
      for(long i = 0; i < n; i++) {
        if(rx_fifo[wr + i] == 3) // Ctrl-C
          host_exit(130);
      }
    }
    rx_fifo_wrptr += n;
  }
  task_wakeup(&console_rx, 1);
}


static ssize_t
console_read(struct stream *s, void *buf, size_t size, size_t reqsize)
{
  char *d = buf;

  if(!can_sleep()) {
    // Panic console or similar: interrupts are off, poll the fd directly
    size_t i = 0;
    while(i < size) {
      long n = linux_syscall(SYS_read, STDIN_FD, d + i, 1);
      if(n == -LINUX_EINTR)
        continue;
      if(n == 0 && i == 0)
        return ERR_NOT_CONNECTED; // EOF, let the (panic) console give up
      if(n <= 0)
        break;
      i++;
      if(i >= reqsize)
        break;
    }
    return i;
  }

  int q = irq_forbid(IRQ_LEVEL_CONSOLE);

  for(size_t i = 0; i < size; i++) {
    while(1) {
      uint8_t avail = rx_fifo_wrptr - rx_fifo_rdptr;
      if(avail)
        break;
      if(console_eof) {
        irq_permit(q);
        host_exit(0);
      }
      if(console_rx_more) {
        console_rx_more = 0;
        host_irq_raise(console_irq);
        continue;
      }
      if(i >= reqsize) {
        irq_permit(q);
        return i;
      }
      task_sleep(&console_rx);
    }

    d[i] = rx_fifo[rx_fifo_rdptr & (RX_FIFO_SIZE - 1)];
    rx_fifo_rdptr++;
  }
  irq_permit(q);
  return size;
}


static ssize_t
console_write(struct stream *s, const void *buf, size_t size, int flags)
{
  const uint8_t *d = buf;
  size_t off = 0;
  while(off < size) {
    long n = linux_syscall(SYS_write, STDOUT_FD, d + off, size - off);
    if(n == -LINUX_EINTR)
      continue;
    if(n == -LINUX_EAGAIN) {
      linux_poll1(STDOUT_FD, POLLOUT, NULL);
      continue;
    }
    if(n < 0)
      break;
    off += n;
  }
  return size;
}


static const stream_vtable_t console_vtable = {
  .read = console_read,
  .write = console_write
};

static stream_t console_stream = { &console_vtable };


static void __attribute__((constructor(110)))
console_init(void)
{
  stdio = &console_stream;

  // Test suites are driven by code, not by stdin. Leave the terminal
  // alone and don't wire up input interrupts.
  if(host_test_mode())
    return;

  if(linux_syscall(SYS_ioctl, STDIN_FD, TCGETS, &console_saved_termios) == 0) {
    console_is_tty = 1;
    struct linux_termios raw = console_saved_termios;
    raw.c_lflag &= ~(ICANON | ECHO | ISIG | IEXTEN);
    raw.c_iflag &= ~(ICRNL | IXON);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    linux_syscall(SYS_ioctl, STDIN_FD, TCSETS, &raw);
  }

  task_waitable_init(&console_rx, "console");

  console_irq = host_irq_alloc(IRQ_LEVEL_CONSOLE, console_irq_handler, NULL);
  host_irq_attach_fd(console_irq, STDIN_FD);

  // Pick up anything already buffered (piped input)
  host_irq_raise(console_irq);
}


static void __attribute__((destructor(110)))
console_fini(void)
{
  if(host_test_mode())
    return;
  host_irq_detach_fd(STDIN_FD);
  if(console_is_tty)
    linux_syscall(SYS_ioctl, STDIN_FD, TCSETS, &console_saved_termios);
}
