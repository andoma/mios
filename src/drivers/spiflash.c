#include "spiflash.h"

#include <mios/block.h>
#include <mios/mios.h>
#include <mios/task.h>
#include <mios/eventlog.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/uio.h>
#include <malloc.h>
#include <unistd.h>

typedef struct spiflash {
  block_iface_t iface;
  spi_t *spi;
  mutex_t mutex;
  int64_t busy_until;
  uint32_t sectors;
  uint32_t spicfg;
  gpio_t cs;
  uint8_t state;
  uint8_t tx[9];
  uint8_t rx[9];
} spiflash_t;

#define SPIFLASH_STATE_IDLE 0
#define SPIFLASH_STATE_BUSY 1
#define SPIFLASH_STATE_PD   2
#define SPIFLASH_STATE_OFF  3


static int
spiflash_get_status(spiflash_t *sf)
{
  sf->tx[0] = 5;
  sf->tx[1] = 0;
  error_t err = sf->spi->rw(sf->spi, sf->tx, sf->rx, 2, sf->cs, sf->spicfg);
  if(err)
    return err;
  return sf->rx[1];
}

static int
spiflash_id(spiflash_t *sf)
{
  memset(sf->tx, 0, 5);
  sf->tx[0] = 0xab;
  error_t err = sf->spi->rw(sf->spi, sf->tx, sf->rx, 5, sf->cs, sf->spicfg);
  if(err)
    return err;
  return sf->rx[4];
}


static error_t
spiflash_wait_ready(spiflash_t *sf)
{
  int status;

  switch(sf->state) {
  case SPIFLASH_STATE_OFF:
    return ERR_NOT_READY;

  case SPIFLASH_STATE_IDLE:
    return 0;

  case SPIFLASH_STATE_BUSY:
    if(sf->busy_until) {
      sleep_until(sf->busy_until);
      sf->busy_until = 0;
    }
    for(int i = 0 ; i < 500; i++) {
      status = spiflash_get_status(sf);
      if(status < 0)
        return status;
      if((status & 1) == 0) {
        sf->state = SPIFLASH_STATE_IDLE;
        return 0;
      }
      usleep(1000);
    }
    return ERR_FLASH_TIMEOUT;

  case SPIFLASH_STATE_PD:
    status = spiflash_id(sf);
    if(status < 0)
      return status;
    sf->state = SPIFLASH_STATE_IDLE;
    return 0;
  default:
    panic("spiflash: Bad state");
  }
}


static error_t
spiflash_we(spiflash_t *sf)
{
  sf->tx[0] = 0x6;
  return sf->spi->rw(sf->spi, sf->tx, NULL, 1, sf->cs, sf->spicfg);
}


static error_t
spiflash_erase(struct block_iface *bi, size_t block)
{
  spiflash_t *sf = (spiflash_t *)bi;

  error_t err = spiflash_wait_ready(sf);
  if(!err) {
    err = spiflash_we(sf);
  }

  if(!err) {
    uint32_t addr = block * bi->block_size;
    sf->tx[0] = 0x20;
    sf->tx[1] = addr >> 16;
    sf->tx[2] = addr >> 8;
    sf->tx[3] = addr;
    err = sf->spi->rw(sf->spi, sf->tx, NULL, 4, sf->cs, sf->spicfg);
    sf->state = SPIFLASH_STATE_BUSY;
    sf->busy_until = clock_get() + 45000;
  }

  return err;
}


static error_t
spiflash_write(struct block_iface *bi, size_t block,
               size_t offset, const void *data, size_t length)
{
  spiflash_t *sf = (spiflash_t *)bi;
  const size_t page_size = 256;

  error_t err = 0;
  while(length) {

    if((err = spiflash_wait_ready(sf)) != 0)
      break;
    if((err = spiflash_we(sf)) != 0)
      break;

    size_t to_copy = length;

    if(to_copy > page_size)
      to_copy = page_size;

    size_t last_byte = offset + to_copy - 1;
    if((last_byte & ~(page_size - 1)) != (offset & ~(page_size - 1))) {
      to_copy = page_size - (offset & (page_size - 1));
    }

    uint32_t addr = block * bi->block_size + offset;

    sf->tx[0] = 0x2;
    sf->tx[1] = addr >> 16;
    sf->tx[2] = addr >> 8;
    sf->tx[3] = addr;

    struct iovec tx[2] = {{sf->tx, 4}, {(void *)data, to_copy}};
    err = sf->spi->rwv(sf->spi, tx, NULL, 2, sf->cs, sf->spicfg);
    sf->state = SPIFLASH_STATE_BUSY;

    if(err)
      break;

    length -= to_copy;
    data += to_copy;
    offset += to_copy;
  }
  return err;
}


static error_t
spiflash_read(struct block_iface *bi, size_t block,
              size_t offset, void *data, size_t length)
{
  spiflash_t *sf = (spiflash_t *)bi;

  error_t err = spiflash_wait_ready(sf);
  if(!err) {

    uint32_t addr = block * bi->block_size + offset;

    sf->tx[0] = 0x3;
    sf->tx[1] = addr >> 16;
    sf->tx[2] = addr >> 8;
    sf->tx[3] = addr;

    struct iovec tx[2] = {{sf->tx, 4}, {NULL, length}};
    struct iovec rx[2] = {{NULL, 4}, {data, length}};
    err = sf->spi->rwv(sf->spi, tx, rx, 2, sf->cs, sf->spicfg);
  }
  return err;
}


static error_t
spiflash_pd(spiflash_t *sf)
{
  sf->tx[0] = 0xb9;
  return sf->spi->rw(sf->spi, sf->tx, NULL, 1, sf->cs, sf->spicfg);
}

static error_t
spiflash_ctrl(struct block_iface *bi, block_ctrl_op_t op)
{
  spiflash_t *sf = (spiflash_t *)bi;

  error_t err;
  switch(op) {

  case BLOCK_LOCK:
    mutex_lock(&sf->mutex);
    return 0;

  case BLOCK_UNLOCK:
    mutex_unlock(&sf->mutex);
    return 0;

  case BLOCK_SYNC:
    return spiflash_wait_ready(sf);

  case BLOCK_SUSPEND:
  case BLOCK_SHUTDOWN:
    err = spiflash_wait_ready(sf);
    if(!err) {
      err = spiflash_pd(sf);
      sf->state = op == BLOCK_SHUTDOWN ? SPIFLASH_STATE_OFF : SPIFLASH_STATE_PD;
    }
    return err;
  default:
    return ERR_OPERATION_FAILED;
  }
}


static uint32_t
read_sfdp(spiflash_t *sf, uint32_t addr)
{
  memset(sf->tx, 0, 9);
  sf->tx[0] = 0x5a;
  sf->tx[3] = addr;
  error_t err = sf->spi->rw(sf->spi, sf->tx, sf->rx, 9,
                            sf->cs, sf->spicfg);
  if(err)
    return 0;

  uint32_t r;
  memcpy(&r, sf->rx + 5, 4);
  return r;
}


block_iface_t *
spiflash_create(spi_t *spi, gpio_t cs)
{
  spiflash_t *sf = xalloc(sizeof(spiflash_t), 0, MEM_TYPE_DMA | MEM_MAY_FAIL);
  if(sf == NULL)
    return NULL;
  memset(sf, 0, sizeof(spiflash_t));
  gpio_conf_output(cs, GPIO_PUSH_PULL, GPIO_SPEED_LOW, GPIO_PULL_NONE);
  gpio_set_output(cs, 1);

  sf->spi = spi;
  sf->cs = cs;

  sf->spicfg = spi->get_config(spi, 0, 10000000);

  int id = spiflash_id(sf);
  printf("spiflash: ID:0x%x  ", id);
  if(id < 0)
    goto bad;

  uint32_t sfdp_signature = read_sfdp(sf, 0);
  if(sfdp_signature != 0x50444653) {
    printf("Invalid SFDP signature [0x%x] ", sfdp_signature);
    goto bad;
  }
  uint32_t hdr2 = read_sfdp(sf, 4);
  int nph = ((hdr2 >> 16) & 0xff);

  uint32_t fpaddr = 0;
  for(int i = 0; i <= nph; i++) {
    uint32_t ph1 = read_sfdp(sf, 0x8 + i * 8);
    uint32_t ph2 = read_sfdp(sf, 0xc + i * 8);
    if((ph1 & 0xff) == 0)
      fpaddr = ph2 & 0xffffff;
  }

  if(fpaddr == 0) {
    printf("Missing Flash Parameters  ");
    goto bad;
  }

  uint32_t density = read_sfdp(sf, fpaddr + 4);
  uint32_t size = 0;
  if(density & 0x80000000) {
    printf("Unsupported density %x  ", density);
    goto bad;
  }

  size = (density + 1) >> 3;
  sf->sectors = size / 4096;

  sf->iface.num_blocks = size / 4096;
  sf->iface.block_size = 4096;

  printf("%d kB (%zd sectors)  ", size >> 10, sf->iface.num_blocks);
  mutex_init(&sf->mutex, "spiflash");
  sf->iface.erase = spiflash_erase;
  sf->iface.write = spiflash_write;
  sf->iface.read = spiflash_read;
  sf->iface.ctrl = spiflash_ctrl;
  printf("OK\n");
  return &sf->iface;

 bad:
  free(sf);
  printf("Not configured\n");
  return NULL;
}
