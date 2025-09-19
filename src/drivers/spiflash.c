#include "spiflash.h"

#include <mios/block.h>
#include <mios/mios.h>
#include <mios/task.h>
#include <mios/eventlog.h>
#include <mios/device.h>
#include <mios/type_macros.h>

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/uio.h>
#include <malloc.h>
#include <unistd.h>

typedef struct spiflash {
  block_iface_t iface;
  device_t dev;
  spi_t *spi;
  mutex_t mutex;
  int64_t busy_until;
  uint32_t sectors;
  uint32_t spicfg;

  struct {
    uint8_t size; // in 2^n
    uint8_t cmd3; // 0xff means not supported
    uint8_t cmd4; // 0xff means not supported
    uint16_t delay;
  } erase_commands[4];

  gpio_t cs;
  uint8_t state;
  uint8_t block_shift;

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
  struct iovec tx[2] = {{sf->tx, 1}, {NULL, 1}};
  struct iovec rx[2] = {{NULL, 0}, {sf->rx, 0}};

  sf->tx[0] = 5;
  error_t err = sf->spi->rwv(sf->spi, tx, rx, 2, sf->cs, sf->spicfg);
  if(err)
    return err;
  return sf->rx[0];
}

static int
spiflash_id(spiflash_t *sf)
{
  struct iovec tx[2] = {{sf->tx, 1}, {NULL, 4}};
  struct iovec rx[2] = {{NULL, 0}, {sf->rx, 0}};

  sf->tx[0] = 0xab;
  error_t err = sf->spi->rwv(sf->spi, tx, rx, 2, sf->cs, sf->spicfg);
  if(err)
    return err;
  return sf->rx[3];
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


static int
wrcmd(uint8_t *dst, uint32_t addr, uint8_t cmd3, uint8_t cmd4)
{
  int longform = addr > 0xffffff;
  uint8_t *ap = longform ? dst + 1 : dst;
  ap[0] = addr >> 24;
  ap[1] = addr >> 16;
  ap[2] = addr >> 8;
  ap[3] = addr;
  dst[0] = longform ? cmd4 : cmd3;
  return 4 + longform;
}

static error_t
spiflash_erase(struct block_iface *bi, size_t block, size_t count)
{
  spiflash_t *sf = (spiflash_t *)bi;
  error_t err;

  while(count) {

    if((err = spiflash_wait_ready(sf)) != 0)
      return err;

    if((err = spiflash_we(sf)) != 0)
      return err;

    const uint32_t addr = block << sf->block_shift;

    err = ERR_INVALID_ADDRESS;
    for(int i = 3; i >= 0; i--) {
      if(sf->erase_commands[i].size == 0)
        continue;

      size_t chunk = 1 << (sf->erase_commands[i].size - sf->block_shift);
      if(chunk > count)
        continue;

      uint32_t mask = (1 << sf->erase_commands[i].size) - 1;
      if(addr & mask) {
        continue;
      }
      int cmdlen = wrcmd(sf->tx, addr, sf->erase_commands[i].cmd3,
                         sf->erase_commands[i].cmd4);
      if(sf->tx[0] == 0xff) {
        continue;
      }

      err = sf->spi->rw(sf->spi, sf->tx, NULL, cmdlen, sf->cs, sf->spicfg);
      sf->state = SPIFLASH_STATE_BUSY;
      sf->busy_until = clock_get() + sf->erase_commands[i].delay * 1000;
      block += chunk;
      count -= chunk;

      break;
    }

    if(err)
      return err;

  }

  return 0;
}


static int
is_all_ones(const uint8_t *data, size_t len)
{
  for(size_t i = 0 ; i < len; i++)
    if(data[i] != 0xff)
      return 0;
  return 1;
}

static error_t
spiflash_write(struct block_iface *bi, size_t block,
               size_t offset, const void *data, size_t length)
{
  spiflash_t *sf = (spiflash_t *)bi;
  const size_t page_size = 256;

  error_t err = 0;
  while(length) {

    size_t to_copy = length;

    if(to_copy > page_size)
      to_copy = page_size;

    size_t last_byte = offset + to_copy - 1;
    if((last_byte & ~(page_size - 1)) != (offset & ~(page_size - 1))) {
      to_copy = page_size - (offset & (page_size - 1));
    }

    if(!is_all_ones(data, to_copy)) {

      if((err = spiflash_wait_ready(sf)) != 0)
        break;
      if((err = spiflash_we(sf)) != 0)
        break;

      const uint32_t addr = block * bi->block_size + offset;
      const int cmdlen = wrcmd(sf->tx, addr, 0x2, 0x12);
      struct iovec tx[2] = {{sf->tx, cmdlen}, {(void *)data, to_copy}};
      err = sf->spi->rwv(sf->spi, tx, NULL, 2, sf->cs, sf->spicfg);
      sf->state = SPIFLASH_STATE_BUSY;

      if(err)
        break;
    }

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

    const uint32_t addr = block * bi->block_size + offset;
    const int cmdlen = wrcmd(sf->tx, addr, 0x3, 0x13);
    struct iovec tx[2] = {{sf->tx, cmdlen}, {NULL, length}};
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
  struct iovec tx[2] = {{sf->tx, 4}, {NULL, 5}};
  struct iovec rx[2] = {{NULL, 0}, {sf->rx, 0}};

  memset(sf->tx, 0, 4);
  sf->tx[0] = 0x5a;
  sf->tx[3] = addr;

  error_t err = sf->spi->rwv(sf->spi, tx, rx, 2, sf->cs, sf->spicfg);
  if(err)
    return 0;

  uint32_t r;
  memcpy(&r, sf->rx + 1, 4);
  return r;
}


static void
spiflash_print_info(struct device *dev, struct stream *s)
{
  spiflash_t *sf = container_of(dev, spiflash_t, dev);

  stprintf(s, "\tSector size:%u  Total sectors:%u  Total size:%u kB\n",
           1 << sf->block_shift,
           sf->sectors,
           ((1 << sf->block_shift) * sf->sectors) / 1024);

  for(size_t i = 0; i < 4; i++) {
    if(!sf->erase_commands[i].size)
      continue;
    stprintf(s, "\tErase size:%6d cmd3:0x%02x cmd4:0x%02x erasetime:%d ms\n",
             1 << sf->erase_commands[i].size,
             sf->erase_commands[i].cmd3,
             sf->erase_commands[i].cmd4,
             sf->erase_commands[i].delay);
  }
}

static const device_class_t spiflash_device_class = {
  .dc_print_info = spiflash_print_info,
};


static const uint16_t erase_time_multipliers[4] = {1,16,128,1000};

static int
calc_erase_time(uint32_t v)
{
  return (1 + (v & 0x1f)) * erase_time_multipliers[(v >> 5) & 3];
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

  uint32_t ptpj = 0;
  uint32_t ptp4 = 0;
  for(int i = 0; i <= nph; i++) {
    uint32_t ph1 = read_sfdp(sf, 0x8 + i * 8);
    uint32_t ph2 = read_sfdp(sf, 0xc + i * 8);
    if((ph1 & 0xff) == 0)
      ptpj = ph2 & 0xffffff;
    if((ph1 & 0xff) == 0x84)
      ptp4 = ph2 & 0xffffff;
  }

  if(ptpj == 0) {
    printf("Missing Flash Parameters  ");
    goto bad;
  }

  uint32_t density = read_sfdp(sf, ptpj + 4);
  uint32_t size = 0;
  if(density & 0x80000000) {
    printf("Unsupported density %x  ", density);
    goto bad;
  }

  size = (density + 1) >> 3;
  sf->block_shift = 12;

  sf->sectors = size >> sf->block_shift;
  sf->iface.num_blocks = size >> sf->block_shift;
  sf->iface.block_size = 1 << sf->block_shift;

  printf("%d kB (%zd sectors)  ", size >> 10, sf->iface.num_blocks);


  uint32_t w = read_sfdp(sf, ptpj + 0x1c);

  sf->erase_commands[0].size = w;
  sf->erase_commands[0].cmd3 = w >> 8;
  sf->erase_commands[1].size = w >> 16;
  sf->erase_commands[1].cmd3 = w >> 24;

  w = read_sfdp(sf, ptpj + 0x20);
  sf->erase_commands[2].size = w;
  sf->erase_commands[2].cmd3 = w >> 8;
  sf->erase_commands[3].size = w >> 16;
  sf->erase_commands[3].cmd3 = w >> 24;

  w = read_sfdp(sf, ptpj + 0x24);
  for(int i = 0; i < 4; i++) {
    sf->erase_commands[i].delay = calc_erase_time(w >> (4 + i * 7));
  }

  if(ptp4) {
    w = read_sfdp(sf, ptp4 + 0x4);
    sf->erase_commands[0].cmd4 = w;
    sf->erase_commands[1].cmd4 = w >> 8;
    sf->erase_commands[2].cmd4 = w >> 16;
    sf->erase_commands[3].cmd4 = w >> 24;
  } else {
    sf->erase_commands[0].cmd4 = 0xff;
    sf->erase_commands[1].cmd4 = 0xff;
    sf->erase_commands[2].cmd4 = 0xff;
    sf->erase_commands[3].cmd4 = 0xff;
  }

  mutex_init(&sf->mutex, "spiflash");
  sf->iface.erase = spiflash_erase;
  sf->iface.write = spiflash_write;
  sf->iface.read = spiflash_read;
  sf->iface.ctrl = spiflash_ctrl;
  printf("OK\n");

  sf->dev.d_name = "spiflash";
  sf->dev.d_class = &spiflash_device_class;
  device_register(&sf->dev);

  return &sf->iface;

 bad:
  free(sf);
  printf("Not configured\n");
  return NULL;
}
