#include <assert.h>
#include <stdio.h>

#include "mios.h"
#include "irq.h"
#include "task.h"

#include "platform.h"
#include "reg.h"
#include "gpio.h"



#define USART_SR   0x00
#define USART_DR   0x04
#define USART_BBR  0x08
#define USART_CR1  0x0c




#define BAUDRATE 115200
#define BBR_VALUE ((SYSTICK_RVR + (BAUDRATE - 1)) / BAUDRATE)

#define TX_FIFO_SIZE 128
#define RX_FIFO_SIZE 64


typedef struct {

  uint32_t reg_base;

  struct task_queue wait_rx;
  struct task_queue wait_tx;

  uint8_t rx_fifo_rdptr;
  uint8_t rx_fifo_wrptr;
  uint8_t tx_fifo_rdptr;
  uint8_t tx_fifo_wrptr;
  uint8_t tx_busy;

  uint8_t tx_fifo[TX_FIFO_SIZE];
  uint8_t rx_fifo[RX_FIFO_SIZE];

} uart_t;

#define CR1_IDLE       (1 << 13) | (1 << 5) | (1 << 3) | (1 << 2)
#define CR1_ENABLE_TXI CR1_IDLE | (1 << 7)

static void
uart_putc(void *arg, char c)
{
  uart_t *u = arg;

  uint32_t primask;
  asm volatile ("mrs %0, primask\n\t" : "=r" (primask));
  int s = irq_forbid(IRQ_LEVEL_CONSOLE);

  if(primask || s) {
    // We are in an interrupt or all interrupts disabled, we busy wait
    reg_wr(u->reg_base + USART_DR, c);
    while(!(reg_rd(u->reg_base + USART_SR) & (1 << 7))) {}
    irq_permit(s);
    return;
  }

  while(1) {
    uint8_t avail = TX_FIFO_SIZE - (u->tx_fifo_wrptr - u->tx_fifo_rdptr);

    if(avail)
      break;
    assert(u->tx_busy);
    task_sleep(&u->wait_tx, 0);
  }

  if(!u->tx_busy) {
    reg_wr(u->reg_base + USART_DR, c);
    reg_wr(u->reg_base + USART_CR1, CR1_ENABLE_TXI);
    u->tx_busy = 1;
  } else {
    u->tx_fifo[u->tx_fifo_wrptr & (TX_FIFO_SIZE - 1)] = c;
    u->tx_fifo_wrptr++;
  }
  irq_permit(s);
}




static int
uart_getc(void *arg)
{
  uart_t *u = arg;

  int s = irq_forbid(IRQ_LEVEL_CONSOLE);

  while(u->rx_fifo_wrptr == u->rx_fifo_rdptr)
    task_sleep(&u->wait_rx, 0);

  char c = u->rx_fifo[u->rx_fifo_rdptr & (RX_FIFO_SIZE - 1)];
  u->rx_fifo_rdptr++;
  irq_permit(s);
  return c;
}




static void
uart_irq(uart_t *u)
{
  const uint32_t sr = reg_rd(u->reg_base + USART_SR);

  if(sr & (1 << 5)) {
    const uint8_t c = reg_rd(u->reg_base + USART_DR);
    u->rx_fifo[u->rx_fifo_wrptr & (RX_FIFO_SIZE - 1)] = c;
    u->rx_fifo_wrptr++;
    task_wakeup(&u->wait_rx, 1);
  }

  if(sr & (1 << 7)) {
    uint8_t avail = u->tx_fifo_wrptr - u->tx_fifo_rdptr;
    if(avail == 0) {
      u->tx_busy = 0;
      reg_wr(u->reg_base + USART_CR1, CR1_IDLE);
    } else {
      uint8_t c = u->tx_fifo[u->tx_fifo_rdptr & (TX_FIFO_SIZE - 1)];
      u->tx_fifo_rdptr++;
      task_wakeup(&u->wait_tx, 1);
      reg_wr(u->reg_base + USART_DR, c);
    }
  }
}


static void
uart_init(uart_t *u, int reg_base, int baudrate)
{
  const unsigned int bbr = (APB1CLOCK + baudrate - 1) / baudrate;

  u->reg_base = reg_base;
  reg_wr(u->reg_base + USART_CR1, (1 << 13)); // ENABLE
  reg_wr(u->reg_base + USART_BBR, bbr);
  reg_wr(u->reg_base + USART_CR1, CR1_IDLE);
  TAILQ_INIT(&u->wait_rx);
  TAILQ_INIT(&u->wait_tx);
}




static uart_t console;

void
irq_38(void)
{
  uart_t *u = &console;
  //  gpio_set_output(GPIO_D, 13, 1);
  uart_irq(u);
  //  gpio_set_output(GPIO_D, 13, 0);
}




static void __attribute__((constructor(110)))
console_init_early(void)
{
  // Configure PA2 for USART2 TX (Alternative Function 7)
  gpio_conf_af(GPIO_A, 2, 7, GPIO_SPEED_HIGH, GPIO_PULL_NONE);
  gpio_conf_af(GPIO_A, 3, 7, GPIO_SPEED_HIGH, GPIO_PULL_UP);

  uart_init(&console, 0x40004400, 115200);

  irq_enable(38, IRQ_LEVEL_CONSOLE);

  init_printf(&console, uart_putc);
  init_getchar(&console, uart_getc);
}


#if 0
static void *
console_status(void *a)
{
  sleephz(HZ / 2);
  while(1) {
    sleephz(HZ);

    int s = irq_forbid(IRQ_LEVEL_CONSOLE);
    int a = console.stalls;
    int b = console.starts;
    int c = console.interrupts;
    irq_permit(s);
    printf("%d %d %d\n", a, b, c);
  }
  return NULL;
}



static void __attribute__((constructor(1000)))
console_init_status(void)
{
  task_create(console_status, NULL, 512, "status");
}
#endif
