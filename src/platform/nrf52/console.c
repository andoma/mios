#include <stdio.h>

#include "mios.h"
#include "irq.h"
#include "task.h"

#include "platform.h"


static volatile unsigned int * const UART_INTENSET = (unsigned int *)0x40002304;
static volatile unsigned int * const UART_ENABLE   = (unsigned int *)0x40002500;
static volatile unsigned int * const UART_PSELTXD  = (unsigned int *)0x4000250c;
static volatile unsigned int * const UART_PSELRXD  = (unsigned int *)0x40002514;
static volatile unsigned int * const UART_TXD      = (unsigned int *)0x4000251c;
static volatile unsigned int * const UART_RXD      = (unsigned int *)0x40002518;
static volatile unsigned int * const UART_BAUDRATE = (unsigned int *)0x40002524;
static volatile unsigned int * const UART_TX_TASK  = (unsigned int *)0x40002008;
static volatile unsigned int * const UART_TX_RDY   = (unsigned int *)0x4000211c;

static volatile unsigned int * const UART_RX_TASK  = (unsigned int *)0x40002000;
static volatile unsigned int * const UART_RX_RDY   = (unsigned int *)0x40002108;

#if 0
static void
uart_putc(void *p, char c)
{
  uint32_t control;
  asm volatile ("mrs %0, control\n\t" : "=r" (control));

  if(!(control & 1)) {
    // On main stack (ie, we are in an IRQ), busy wait write
    *UART_TXD = c;
    *UART_TX_TASK = 1;
    while(!*UART_TX_RDY) {
    }
    *UART_TX_RDY = 0;
    *UART_TX_TASK = 0;
  }
}
#endif

static void
uart_putc(void *p, char c)
{
  *UART_TXD = c;
  *UART_TX_TASK = 1;
  while(!*UART_TX_RDY) {
  }
  *UART_TX_RDY = 0;
  *UART_TX_TASK = 0;
}



void
platform_console_init_early(void)
{
  *UART_PSELTXD = 6;
  *UART_PSELRXD = 8;
  *UART_ENABLE = 4;
  *UART_BAUDRATE = 0x1d60000;
  init_printf(NULL, uart_putc);
}


static struct task_queue uart_waitable = TAILQ_HEAD_INITIALIZER(uart_waitable);

static uint8_t rx_fifo_rdptr;
static uint8_t rx_fifo_wrptr;
static uint8_t rx_fifo[64];



void
irq_2(void)
{
  //  printf("RX %d\n", *UART_RXD);
  //  irq_ack(2);

  *UART_RX_RDY = 0;

  rx_fifo[rx_fifo_wrptr & 63] = *UART_RXD;
  rx_fifo_wrptr++;
  task_wakeup(&uart_waitable, 1);
}


static void *
console_task(void *arg)
{
  int s = irq_forbid(IRQ_LEVEL_CONSOLE);

  while(1) {

    uint8_t avail = rx_fifo_wrptr - rx_fifo_rdptr;
    if(avail == 0) {
      task_sleep(&uart_waitable, 0);
      continue;
    }

    char c = rx_fifo[rx_fifo_rdptr & 63];
    rx_fifo_rdptr++;

    irq_permit(s);
    printf("RX: %d\n", c);
    s = irq_forbid(IRQ_LEVEL_CONSOLE);
  }
  return NULL;
}

void
platform_console_init(void)
{
  //  init_printf(NULL, uart_putc2);
  //  *UART_INTENSET = 0x84; // RXDRDY and TXDRDY
  *UART_INTENSET = 0x4; // RXDRDY
  *UART_RX_TASK = 1;
  irq_enable(2, IRQ_LEVEL_CONSOLE);


  task_create(console_task, NULL, 256, "console");
}
