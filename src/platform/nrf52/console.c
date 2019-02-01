#include <stdio.h>

#include "irq.h"

#include "platform.h"


static volatile unsigned int * const UART_ENABLE   = (unsigned int *)0x40002500;
static volatile unsigned int * const UART_PSELTXD  = (unsigned int *)0x4000250c;
static volatile unsigned int * const UART_PSELRXD  = (unsigned int *)0x40002514;
static volatile unsigned int * const UART_TXD      = (unsigned int *)0x4000251c;
static volatile unsigned int * const UART_BAUDRATE = (unsigned int *)0x40002524;
static volatile unsigned int * const UART_TX_TASK  = (unsigned int *)0x40002008;
static volatile unsigned int * const UART_TX_RDY   = (unsigned int *)0x4000211c;

static void
uart_putc(void *p, char c)
{
  //  irq_forbid();
  *UART_TXD = c;
  *UART_TX_TASK = 1;
  while(!*UART_TX_RDY) {
  }
  *UART_TX_RDY = 0;
  *UART_TX_TASK = 0;
  //  irq_permit();
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


}

void
platform_console_init(void)
{


}
