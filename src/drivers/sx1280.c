#include <unistd.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "mios.h"
#include "sx1280.h"
#include "irq.h"
#include "task.h"

#define TXFIFO_SIZE 1024 // Must be power of 2
#define TXFIFO_MASK (TXFIFO_SIZE - 1)


struct sx1280 {
  spi_t *bus;
  gpio_t gpio_nss;
  gpio_t gpio_busy;

  mutex_t mutex;
  cond_t cond_work;
  cond_t cond_txfifo;

  struct task_queue busy_waitable;

  uint8_t pending_irq;
  uint8_t sending;

  uint16_t txfifo_wrptr;
  uint16_t txfifo_rdptr;

  uint8_t txfifo[TXFIFO_SIZE];

  gpio_t gpio_reset;

  uint8_t freqparams[4];
  uint8_t modparams[4];
  uint8_t pktparams[8];
  uint8_t txparams[3];
};


static const signed int cmdstatus_to_error[8] = {
  ERR_OPERATION_FAILED,
  ERR_OK,
  ERR_OK,
  ERR_TIMEOUT,
  ERR_OPERATION_FAILED,
  ERR_OPERATION_FAILED,
  ERR_OK,
  ERR_OPERATION_FAILED
};

static const uint8_t use_dcdc[2] = {
  RADIO_SET_REGULATORMODE, USE_DCDC
};

static const uint8_t init_baseaddr[3] = {
  RADIO_SET_BUFFERBASEADDRESS, 0, 0
};

static const uint8_t get_status[1] = {
  RADIO_GET_STATUS
};

static const uint8_t set_flrc[2] = {
  RADIO_SET_PACKETTYPE, PACKET_TYPE_FLRC
};

static const uint8_t init_irq[9] = {
  RADIO_SET_DIOIRQPARAMS,
  0, 3,
  0, 3,
  0, 0,
  0, 0,
};

static const uint8_t clear_irq[3] = {
  RADIO_CLR_IRQSTATUS, 0xff, 0xff
};


static error_t
wait_ready(sx1280_t *s)
{
  while(gpio_get_input(s->gpio_busy)) {
    if(task_sleep(&s->busy_waitable, 100000))
      return ERR_TIMEOUT;
  }
  return ERR_OK;
}

static error_t
issue_command(sx1280_t *s, const uint8_t *tx, uint8_t *rx, size_t len)
{
  error_t err;

  if((err = wait_ready(s)) != ERR_OK) {
    printf("sx1280: timeout in cmd:%x\n", tx[0]);
    return err;
  }

  error_t error = s->bus->rw(s->bus, tx, rx, len, s->gpio_nss);
  if(!error) {
    uint8_t response;

    // too fast?
    if((err = wait_ready(s)) != ERR_OK) {
      printf("sx1280: timeout in getStatus\n");
      return err;
    }
    error = s->bus->rw(s->bus, get_status, &response, 1, s->gpio_nss);
    if(!error) {
      error = cmdstatus_to_error[(response >> 2) & 7];
      if(error) {
        printf("sx1280: cmd:0x%x err:0x%x\n", tx[0], response);
      }
    }
  }
  return error;
}

static int
sx1280_status(sx1280_t *s)
{
  error_t err;
  uint8_t response;

  if((err = wait_ready(s)) != ERR_OK)
    return err;

  err = s->bus->rw(s->bus, get_status, &response, 1, s->gpio_nss);
  if(err)
    return err;
  return response;
}



static error_t
sx1280_reset(sx1280_t *s)
{
  error_t err;

  s->sending = 0;

  gpio_set_output(s->gpio_reset, 0);
  usleep(20000);
  gpio_set_output(s->gpio_reset, 1);
  usleep(20000);

  err = issue_command(s, use_dcdc, NULL, sizeof(use_dcdc));

  if(!err)
    err = issue_command(s, init_baseaddr, NULL, sizeof(init_baseaddr));

  if(!err)
    err = issue_command(s, init_irq, NULL, sizeof(init_irq));

  if(!err)
    err = issue_command(s, set_flrc, NULL, sizeof(set_flrc));

  if(!err)
    err = issue_command(s, s->modparams, NULL, sizeof(s->modparams));

  if(!err)
    err = issue_command(s, s->freqparams, NULL, sizeof(s->freqparams));

  if(!err)
    err = issue_command(s, s->txparams, NULL, sizeof(s->txparams));

  return err;
}


error_t
sx1280_tx(sx1280_t *s, const void *buffer, size_t len)
{
  error_t err;
  s->pktparams[5] = len;
  err = issue_command(s, s->pktparams, NULL, sizeof(s->pktparams));
  if(err)
    return err;

  if((err = wait_ready(s)) != ERR_OK)
    return err;

  err = s->bus->rw(s->bus, buffer, NULL, len + 2, s->gpio_nss);
  if(err)
    return err;

  uint16_t timeout = 0xffff;
  RadioTickSizes_t ts = RADIO_TICK_SIZE_1000_US;
  const uint8_t txcmd[4] = {
    RADIO_SET_TX, ts, timeout >> 8, timeout
  };

  err = issue_command(s, txcmd, NULL, sizeof(txcmd));
  if(!err)
    s->sending = 1;
  return err;
}


error_t
sx1280_irq_read_and_clear(sx1280_t *s)
{
  uint8_t tx[4] = { RADIO_GET_IRQSTATUS };
  uint8_t rx[4];

  error_t err = issue_command(s, tx, rx, sizeof(tx));
  if(err)
    return err;

  if(rx[3] & 1) {
    s->sending = 0;
  }

  return issue_command(s, clear_irq, NULL, sizeof(clear_irq));
}



static void *
sx1280_thread(void *arg)
{
  sx1280_t *s = arg;

  int q = irq_forbid(IRQ_LEVEL_IO);

  while(1) {
    error_t err = sx1280_reset(s);
    if(err) {
      printf("sx1280: Failed to initialize: %d\n", err);
      sleep(1);
      continue;
    } else {
      printf("sx1280: Initialized OK Idle status:0x%x\n",
             sx1280_status(s));
    }

    mutex_lock(&s->mutex);

    while(!err) {

      uint16_t tx_depth = !s->sending ? s->txfifo_wrptr - s->txfifo_rdptr : 0;

      if(!s->pending_irq && !tx_depth) {
        cond_wait(&s->cond_work, &s->mutex);
        continue;
      }

      if(s->pending_irq) {
        s->pending_irq = 0;
        mutex_unlock(&s->mutex);

        err = sx1280_irq_read_and_clear(s);
        if(err)
          printf("Failed to clear IRQ: %d\n", err);
        mutex_lock(&s->mutex);
        continue;
      }

      if(tx_depth) {
        assert(tx_depth > 1);
        uint8_t pkt[66];
        uint16_t rdptr = s->txfifo_rdptr;
        uint8_t len = s->txfifo[rdptr++ & TXFIFO_MASK];
        assert(len <= ((s->txfifo_wrptr - rdptr) & 0xffff));

        for(size_t i = 0; i < len; i++) {
          pkt[2 + i] = s->txfifo[rdptr++ & TXFIFO_MASK];
        }
        s->txfifo_rdptr = rdptr;
        cond_signal(&s->cond_txfifo);

        mutex_unlock(&s->mutex);

        pkt[0] = RADIO_WRITE_BUFFER;
        pkt[1] = 0;

        err = sx1280_tx(s, pkt, len);
        if(err)
          printf("Failed to send: %d\n", err);

        mutex_lock(&s->mutex);
        continue;

      }
    }
    mutex_unlock(&s->mutex);
  }
  irq_permit(q);
  return NULL;
}




void
sx1280_irq(void *arg)
{
  sx1280_t *s = arg;
  s->pending_irq |= 1;
  cond_signal(&s->cond_work);
}


void
sx1280_busy_irq(void *arg)
{
  sx1280_t *s = arg;
  task_wakeup(&s->busy_waitable, 0);
}


sx1280_t *
sx1280_create(spi_t *bus, const sx1280_config_t *cfg)
{
  sx1280_t *s = malloc(sizeof(sx1280_t));
  memset(s, 0, sizeof(sx1280_t));
  mutex_init(&s->mutex);
  cond_init(&s->cond_work);
  cond_init(&s->cond_txfifo);
  TAILQ_INIT(&s->busy_waitable);

  s->bus = bus;
  s->gpio_nss = cfg->gpio_nss;
  s->gpio_busy = cfg->gpio_busy;
  s->gpio_reset = cfg->gpio_reset;

  gpio_set_output(s->gpio_nss, 1);
  gpio_conf_output(s->gpio_nss, GPIO_PUSH_PULL,
                   GPIO_SPEED_HIGH, GPIO_PULL_NONE);
  gpio_set_output(s->gpio_reset, 1);
  gpio_conf_output(s->gpio_reset, GPIO_PUSH_PULL,
                   GPIO_SPEED_HIGH, GPIO_PULL_NONE);

  gpio_conf_irq(s->gpio_busy, GPIO_PULL_NONE, sx1280_busy_irq, s,
                GPIO_BOTH_EDGES, IRQ_LEVEL_IO);

  gpio_conf_irq(cfg->gpio_irq, GPIO_PULL_NONE, sx1280_irq, s,
                GPIO_RISING_EDGE, IRQ_LEVEL_IO);

  s->modparams[0] = RADIO_SET_MODULATIONPARAMS;
  s->modparams[1] = cfg->br;
  s->modparams[2] = cfg->cr;
  s->modparams[3] = cfg->ms;

  s->pktparams[0] = RADIO_SET_PACKETPARAMS;
  s->pktparams[1] = cfg->pl;
  s->pktparams[2] = cfg->swl;
  s->pktparams[3] = cfg->rxm;
  s->pktparams[4] = cfg->lm;
  s->pktparams[5] = 16; // buffer size, dummy for now
  s->pktparams[6] = cfg->crctype;
  s->pktparams[7] = cfg->wm;

  s->txparams[0] = RADIO_SET_TXPARAMS;
  s->txparams[1] = cfg->output_gain + 18;
  s->txparams[2] = RADIO_RAMP_02_US;

  uint32_t f = cfg->frequency * 0.00504123076f;

  s->freqparams[0] = RADIO_SET_RFFREQUENCY;
  s->freqparams[1] = f >> 16;
  s->freqparams[2] = f >> 8;
  s->freqparams[3] = f;

  task_create(sx1280_thread, s, 512, "sx1280", 0);
  return s;
}


error_t
sx1280_send(sx1280_t *s, const void *data, size_t len, int wait)
{
  const uint8_t *u8 = data;
  if(len > 64)
    return ERR_MTU_EXCEEDED;

  mutex_lock(&s->mutex);

  while(1) {
    uint16_t avail = TXFIFO_SIZE - (s->txfifo_wrptr - s->txfifo_rdptr);
    if(avail < len + 1) {
      if(!wait) {
        mutex_unlock(&s->mutex);
        return ERR_NO_BUFFER;
      }
      cond_wait(&s->cond_txfifo, &s->mutex);
      continue;
    }
    assert(avail <= TXFIFO_SIZE);
    uint16_t wrptr = s->txfifo_wrptr;
    s->txfifo[wrptr++ & TXFIFO_MASK] = len;
    for(size_t i = 0; i < len; i++) {
      s->txfifo[wrptr++ & TXFIFO_MASK] = u8[i];
    }
    s->txfifo_wrptr = wrptr;
    cond_signal(&s->cond_work);
    break;
  }
  mutex_unlock(&s->mutex);
  return ERR_OK;
}
