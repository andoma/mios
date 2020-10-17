#include <stdio.h>
#include <irq.h>
#include <task.h>


static volatile unsigned int * const UART_DR    = (unsigned int *)0x4000c000;
static volatile unsigned int * const UART_IMSC  = (unsigned int *)0x4000c038;
//static volatile unsigned int * const UART_RIS   = (unsigned int *)0x4000c03c;

static task_waitable_t uart_rx;

static uint8_t rx_fifo_rdptr;
static uint8_t rx_fifo_wrptr;

#define RX_FIFO_SIZE 64
static uint8_t rx_fifo[RX_FIFO_SIZE];


static void
uart_putc(void *p, char c)
{
  *(volatile int *)p = c;
}


static int
uart_getc(void *arg)
{
  int s = irq_forbid(IRQ_LEVEL_CONSOLE);

  while(1) {
    uint8_t avail = rx_fifo_wrptr - rx_fifo_rdptr;
    if(avail)
      break;
    task_sleep(&uart_rx);
  }

  char c = rx_fifo[rx_fifo_rdptr & (RX_FIFO_SIZE - 1)];
  rx_fifo_rdptr++;
  irq_permit(s);
  return c;
}


static void __attribute__((constructor(110)))
platform_console_init_early(void)
{
  init_printf((void *)UART_DR, uart_putc);
}

void
irq_5(void)
{
  uint8_t ch = *UART_DR;
  rx_fifo[rx_fifo_wrptr & (RX_FIFO_SIZE - 1)] = ch;
  rx_fifo_wrptr++;
  task_wakeup(&uart_rx, 1);
}


static void  __attribute__((constructor(200)))
lm3s811evb_console_init(void)
{
  init_getchar(NULL, uart_getc);

  irq_enable(5, IRQ_LEVEL_CONSOLE);
  *UART_IMSC = 0x10;
}
