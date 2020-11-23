#include <sys/queue.h>
#include <stdio.h>
#include <assert.h>
#include <unistd.h>
#include <stdlib.h>

#include <mios/io.h>
#include <mios/cli.h>

#include "irq.h"

#include "sx1280/sx1280.h"
#include "sx1280/sx1280_i.h"

SLIST_HEAD(sx1280_mios_slist, sx1280_mios);

static struct sx1280_mios_slist sx1280s;

#define BUSY_IRQ

// We read the system clock in interrupt so we need to
// block clock interrupts (for now)
#define SX1280_IRQ_LEVEL IRQ_LEVEL_CLOCK

typedef struct sx1280_mios {

  sx1280_t s;

  uint64_t pending_irq;

  spi_t *bus;

#ifdef BUSY_IRQ
  task_waitable_t busy_waitable;
  uint8_t busy;
#endif

  gpio_t gpio_nss;
  gpio_t gpio_busy;
  gpio_t gpio_irq;
  gpio_t gpio_reset;

  gpio_t gpio_dbg1;
  gpio_t gpio_dbg2;

  SLIST_ENTRY(sx1280_mios) global_link;

} sx1280_mios_t;


#include "cpu.h"

#ifdef BUSY_IRQ

static sx1280_err_t
wait_ready(sx1280_mios_t *sm)
{
  int s = irq_forbid(IRQ_LEVEL_IO);
  const int64_t deadline = clock_get_irq_blocked() + 50000;
  while(sm->busy) {
    if(task_sleep_deadline(&sm->busy_waitable, deadline, 0)) {
      irq_permit(s);
      return ERR_TIMEOUT;
    }
  }
  irq_permit(s);
  return ERR_OK;
}

#else

static sx1280_err_t
wait_ready(sx1280_mios_t *sm)
{
  const int64_t deadline = clock_get() + 100000;
  while(gpio_get_input(sm->gpio_busy)) {
    if(clock_get() > deadline)
      return ERR_TIMEOUT;
  }
  return ERR_OK;
}
#endif

sx1280_err_t
sx1280_wait_ready(sx1280_t *s)
{
  sx1280_mios_t *sm = (sx1280_mios_t *)s;
  return wait_ready(sm);
}


sx1280_err_t
sx1280_cmd(sx1280_t *s, const uint8_t *tx, uint8_t *rx, size_t len)
{
  sx1280_mios_t *sm = (sx1280_mios_t *)s;
  assert(len > 0);

  error_t err;

  if((err = wait_ready(sm)) != ERR_OK) {
    printf("sx1280: timeout cmd:%x\n", tx[0]);
    return err;
  }
#if 0
  printf("SPI_TX: ");
  for(int i = 0; i < len; i++)
    printf("%02x ", tx[i]);
  printf("\n");
#endif
  err = sm->bus->rw(sm->bus, tx, rx, len, sm->gpio_nss);

#if 0
  if(rx != NULL) {
    printf("SPI_RX: ");
    for(int i = 0; i < len; i++)
      printf("%02x ", rx[i]);
    printf("\n");
  }
#endif
  return err;
}

sx1280_err_t
sx1280_reset(sx1280_t *s)
{
  uint8_t status = RADIO_GET_STATUS;

  sx1280_mios_t *sm = (sx1280_mios_t *)s;

  gpio_set_output(sm->gpio_reset, 0);
  usleep_hr(2000);
  sm->bus->lock(sm->bus, 1);
  gpio_set_output(sm->gpio_reset, 1);

  sx1280_err_t err = wait_ready(sm);
  if(!err)
    err = sm->bus->rw_locked(sm->bus, &status, &status, 1, sm->gpio_nss);

  sm->bus->lock(sm->bus, 0);
  return err;
}


void
sx1280_irq(void *arg)
{
  sx1280_mios_t *sm = arg;
  int irq = gpio_get_input(sm->gpio_irq);
  sm->pending_irq = irq ? clock_get() : 0;
  cond_signal(&sm->s.cond_work);
}


#ifdef BUSY_IRQ
void
sx1280_busy_irq(void *arg)
{
  sx1280_mios_t *sm = arg;
  sm->busy = gpio_get_input(sm->gpio_busy);
  if(!sm->busy) // Wakeup on falling edge only
    task_wakeup(&sm->busy_waitable, 1);
}
#endif


sx1280_t *
sx1280_create(spi_t *bus, gpio_t nss, gpio_t busy, gpio_t irq, gpio_t reset,
              gpio_t dbg1, gpio_t dbg2)
{
  sx1280_mios_t *sm = calloc(1, sizeof(sx1280_mios_t));

  sm->bus = bus;
  sm->gpio_nss = nss;
  sm->gpio_busy = busy;
  sm->gpio_irq = irq;
  sm->gpio_reset = reset;

  gpio_set_output(sm->gpio_nss, 1);
  gpio_conf_output(sm->gpio_nss, GPIO_PUSH_PULL,
                   GPIO_SPEED_HIGH, GPIO_PULL_NONE);

  gpio_set_output(sm->gpio_reset, 1);
  gpio_conf_output(sm->gpio_reset, GPIO_PUSH_PULL,
                   GPIO_SPEED_HIGH, GPIO_PULL_NONE);

#ifdef BUSY_IRQ
  task_waitable_init(&sm->busy_waitable, "sx1280busy");
  gpio_conf_irq(sm->gpio_busy, GPIO_PULL_NONE, sx1280_busy_irq, sm,
                GPIO_BOTH_EDGES, IRQ_LEVEL_IO);
  sm->busy = gpio_get_input(sm->gpio_busy);
#else
  gpio_conf_input(sm->gpio_busy, GPIO_PULL_NONE);
#endif

  gpio_conf_irq(irq, GPIO_PULL_NONE, sx1280_irq, sm,
                GPIO_BOTH_EDGES, SX1280_IRQ_LEVEL);


  sm->gpio_dbg1 = dbg1;
  gpio_set_output(sm->gpio_dbg1, 0);
  gpio_conf_output(sm->gpio_dbg1, GPIO_PUSH_PULL,
                   GPIO_SPEED_HIGH, GPIO_PULL_NONE);

  sm->gpio_dbg2 = dbg2;
  gpio_set_output(sm->gpio_dbg2, 0);
  gpio_conf_output(sm->gpio_dbg2, GPIO_PUSH_PULL,
                   GPIO_SPEED_HIGH, GPIO_PULL_NONE);

  mutex_init(&sm->s.mutex, "sx1280");
  cond_init(&sm->s.cond_work, "sx1280work");
  cond_init(&sm->s.cond_txfifo, "sx1280txfifo");

  SLIST_INSERT_HEAD(&sx1280s, sm, global_link);

  return &sm->s;
}



sx1280_err_t
sx1280_wait(sx1280_t *s, int64_t deadline)
{
  sx1280_mios_t *sm = (sx1280_mios_t *)s;
  int r = 0;
  int q = irq_forbid(SX1280_IRQ_LEVEL);

  while(sm->pending_irq) {
    uint64_t ts = sm->pending_irq;
    sx1280_mutex_unlock(&s->mutex);
    sx1280_err_t err = sx1280_handle_irq(s, ts);
    sx1280_mutex_lock(&s->mutex);
    if(err || s->irq) {
      irq_permit(q);
      return err;
    }
  }


  if(!s->irq) {
    if(deadline) {
      r = cond_wait_timeout(&s->cond_work, &s->mutex, deadline, TIMER_HIGHRES);
    } else {
      cond_wait(&s->cond_work, &s->mutex);
    }
  }
  irq_permit(q);
  return r;
}


void
sx1280_dbg(sx1280_t *s, int line, int value)
{
  sx1280_mios_t *sm = (sx1280_mios_t *)s;

  switch(line) {
  case 1:
    gpio_set_output(sm->gpio_dbg1, value);
    break;
  case 2:
    gpio_set_output(sm->gpio_dbg2, value);
    break;
  }
}


static int
cmd_sx1280(cli_t *cli, int argc, char **argv)
{
  sx1280_mios_t *sm;
  SLIST_FOREACH(sm, &sx1280s, global_link) {
    sx1280_t *s = &sm->s;
    cli_printf(cli, " Link: %s\n",
           s->link_status == SX1280_LINK_UP ? "Up" :
           s->link_status == SX1280_LINK_SYNCHRONIZING ? "Sync" : "Down");
    cli_printf(cli,
               " RxPackets: %-10u RxBytes: %-10u\n"
               " RxErrors:  %-10u RSSI: %d dBm\n"
               " LLErrors:  %d\n"
               " TxPackets: %-10u TxBytes: %-10u\n",
           s->rx_packets,
           s->rx_bytes,
           s->rx_errors,
           s->rx_rssi,
           s->rx_ll_errors,
           s->tx_packets,
           s->tx_bytes);
  }
  return 0;
}



CLI_CMD_DEF("sx1280", cmd_sx1280);

