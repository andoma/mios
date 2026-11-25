#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <sys/uio.h>

#include <mios/io.h>
#include <mios/task.h>

#include "vgpio.h"
#include "vspi.h"

// See vspi.h

#define VSPI_MAX_DEVS 32

typedef struct vspi_slot {
  gpio_t cs;
  vspi_dev_t *dev;
} vspi_slot_t;

typedef struct vspi {
  spi_t spi;
  mutex_t mutex;
  int num_slots;
  vspi_slot_t slots[VSPI_MAX_DEVS];
} vspi_t;


static int
select_devs(vspi_t *v, vspi_dev_t **sel, int mode)
{
  int n = 0;
  for(int i = 0; i < v->num_slots; i++) {
    const vspi_slot_t *s = &v->slots[i];
    if(s->dev != NULL && vgpio_level(s->cs) == 0 &&
       !vgpio_floating(s->cs)) {
      sel[n++] = s->dev;
      s->dev->ops->select(s->dev, mode);
    }
  }
  return n;
}


static error_t
vspi_rwv_locked(vspi_t *v, const struct iovec *txiov,
                const struct iovec *rxiov, size_t count, gpio_t nss,
                int config)
{
  vspi_dev_t *sel[VSPI_MAX_DEVS];

  if(nss != GPIO_UNUSED)
    gpio_set_output(nss, 0);

  const int n = select_devs(v, sel, config & (SPI_CPOL | SPI_CPHA));

  // Length per segment is txiov[i].iov_len, rx length is ignored, as in
  // the real drivers
  for(size_t i = 0; i < count; i++) {
    const uint8_t *tx = txiov[i].iov_base;
    uint8_t *rx = rxiov ? rxiov[i].iov_base : NULL;
    for(size_t j = 0; j < txiov[i].iov_len; j++) {
      const uint8_t mosi = tx ? tx[j] : 0;
      uint8_t miso = 0xff;
      for(int k = 0; k < n; k++)
        miso &= sel[k]->ops->xfer(sel[k], mosi);
      if(rx)
        rx[j] = miso;
    }
  }

  if(nss != GPIO_UNUSED)
    gpio_set_output(nss, 1);

  for(int k = 0; k < n; k++)
    sel[k]->ops->deselect(sel[k]);
  return 0;
}


static error_t
vspi_rwv(struct spi *bus, const struct iovec *txiov, const struct iovec *rxiov,
         size_t count, gpio_t nss, int config)
{
  vspi_t *v = (vspi_t *)bus;
  mutex_lock(&v->mutex);
  error_t err = vspi_rwv_locked(v, txiov, rxiov, count, nss, config);
  mutex_unlock(&v->mutex);
  return err;
}


static error_t
vspi_rw_locked(struct spi *bus, const uint8_t *tx, uint8_t *rx, size_t len,
               gpio_t nss, int config)
{
  struct iovec txv = { (void *)tx, len };
  struct iovec rxv = { rx, len };
  return vspi_rwv_locked((vspi_t *)bus, &txv, rx ? &rxv : NULL, 1, nss,
                         config);
}


static error_t
vspi_rw(struct spi *bus, const uint8_t *tx, uint8_t *rx, size_t len,
        gpio_t nss, int config)
{
  vspi_t *v = (vspi_t *)bus;
  mutex_lock(&v->mutex);
  error_t err = vspi_rw_locked(bus, tx, rx, len, nss, config);
  mutex_unlock(&v->mutex);
  return err;
}


static void
vspi_lock(struct spi *bus, int acquire)
{
  vspi_t *v = (vspi_t *)bus;
  if(acquire)
    mutex_lock(&v->mutex);
  else
    mutex_unlock(&v->mutex);
}


static int
vspi_get_config(struct spi *bus, int clock_flags, int baudrate)
{
  return clock_flags & (SPI_CPOL | SPI_CPHA);
}


spi_t *
vspi_create(const char *name)
{
  vspi_t *v = calloc(1, sizeof(vspi_t));
  mutex_init(&v->mutex, name);
  v->spi.rw = vspi_rw;
  v->spi.rwv = vspi_rwv;
  v->spi.rw_locked = vspi_rw_locked;
  v->spi.lock = vspi_lock;
  v->spi.get_config = vspi_get_config;
  return &v->spi;
}


error_t
vspi_attach(spi_t *bus, gpio_t cs, vspi_dev_t *dev)
{
  vspi_t *v = (vspi_t *)bus;
  error_t err = 0;

  mutex_lock(&v->mutex);
  for(int i = 0; i < v->num_slots; i++) {
    if(v->slots[i].dev != NULL && v->slots[i].cs == cs) {
      err = ERR_EXIST;
      goto out;
    }
  }
  if(v->num_slots == VSPI_MAX_DEVS) {
    err = ERR_NO_MEMORY;
    goto out;
  }
  v->slots[v->num_slots].cs = cs;
  v->slots[v->num_slots].dev = dev;
  v->num_slots++;
 out:
  mutex_unlock(&v->mutex);
  return err;
}


void
vspi_detach(spi_t *bus, gpio_t cs)
{
  vspi_t *v = (vspi_t *)bus;
  mutex_lock(&v->mutex);
  for(int i = 0; i < v->num_slots; i++) {
    if(v->slots[i].cs == cs)
      v->slots[i].dev = NULL;
  }
  mutex_unlock(&v->mutex);
}
