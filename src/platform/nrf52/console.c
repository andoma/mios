#include <assert.h>
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


static struct task_queue uart_rx = TAILQ_HEAD_INITIALIZER(uart_rx);
static struct task_queue uart_tx = TAILQ_HEAD_INITIALIZER(uart_tx);

static uint8_t rx_fifo_rdptr;
static uint8_t rx_fifo_wrptr;
static uint8_t tx_fifo_rdptr;
static uint8_t tx_fifo_wrptr;

#define TX_FIFO_SIZE 128
#define RX_FIFO_SIZE 64

static uint8_t tx_fifo[TX_FIFO_SIZE];
static uint8_t rx_fifo[RX_FIFO_SIZE];

static uint8_t tx_busy;

static void
uart_putc(void *p, char c)
{
  uint32_t primask;
  asm volatile ("mrs %0, primask\n\t" : "=r" (primask));
  int s = irq_forbid(IRQ_LEVEL_CONSOLE);

  if(primask || s) {
    // We are in an interrupt or all interrupts disabled, we busy wait
    *UART_TXD = c;
    *UART_TX_TASK = 1;
    while(!*UART_TX_RDY) {
    }
    *UART_TX_RDY = 0;
    *UART_TX_TASK = 0;
    irq_permit(s);
    return;
  }

  while(1) {
    uint8_t avail = TX_FIFO_SIZE - (tx_fifo_rdptr - tx_fifo_wrptr);
    if(avail == 0) {
      assert(tx_busy);
      task_sleep(&uart_tx, 0);
      continue;
    }

    if(!tx_busy) {
      *UART_TXD = c;
      *UART_TX_TASK = 1;
      tx_busy = 1;
      break;
    }

    tx_fifo[tx_fifo_wrptr & (TX_FIFO_SIZE - 1)] = c;
    tx_fifo_wrptr++;
    break;
  }
  irq_permit(s);
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





void
irq_2(void)
{
  //  printf("RX %d\n", *UART_RXD);
  //  irq_ack(2);

  if(*UART_RX_RDY) {
    *UART_RX_RDY = 0;
    rx_fifo[rx_fifo_wrptr & (RX_FIFO_SIZE - 1)] = *UART_RXD;
    rx_fifo_wrptr++;
    task_wakeup(&uart_rx, 1);
  }

  if(*UART_TX_RDY) {
    *UART_TX_RDY = 0;

    uint8_t avail = tx_fifo_wrptr - tx_fifo_rdptr;
    if(avail == 0) {
      *UART_TX_TASK = 0;
      tx_busy = 0;
    } else {
      char c = tx_fifo[tx_fifo_rdptr & (TX_FIFO_SIZE - 1)];
      tx_fifo_rdptr++;
      task_wakeup(&uart_tx, 1);
      *UART_TXD = c;
    }
  }
}

void __attribute__((noinline))
console_do_echo(char c)
{
  uart_putc(NULL, c);
}


static void *
console_task(void *arg)
{
  int s = irq_forbid(IRQ_LEVEL_CONSOLE);

  while(1) {

    uint8_t avail = rx_fifo_wrptr - rx_fifo_rdptr;
    if(avail == 0) {
      task_sleep(&uart_rx, 0);
      continue;
    }

    char c = rx_fifo[rx_fifo_rdptr & (RX_FIFO_SIZE - 1)];
    rx_fifo_rdptr++;
    console_do_echo(c);
    irq_permit(s);
    s = irq_forbid(IRQ_LEVEL_CONSOLE);
  }
  return NULL;
}

void
platform_console_init(void)
{
  //  init_printf(NULL, uart_putc2);
  *UART_INTENSET = 0x84; // RXDRDY and TXDRDY
  //*UART_INTENSET = 0x4; // RXDRDY
  *UART_RX_TASK = 1;
  irq_enable(2, IRQ_LEVEL_CONSOLE);


  task_create(console_task, NULL, 256, "console");
}
