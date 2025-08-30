#include "t234ccplex_qspi.h"
#include "t234ccplex_bpmp.h"
#include "t234ccplex_clk.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>

#include <sys/param.h>

#include <mios/mios.h>
#include <mios/cli.h>
#include <mios/task.h>
#include <mios/io.h>
#include <mios/block.h>

#include "irq.h"
#include "reg.h"

#include <drivers/spiflash.h>

#define QSPI_BASE 0x3270000


#define QSPI_COMMAND       (QSPI_BASE + 0x000)
#define QSPI_STATUS        (QSPI_BASE + 0x010)
#define QSPI_FIFO_STATUS   (QSPI_BASE + 0x014)
#define QSPI_BLK_SIZE      (QSPI_BASE + 0x024)
#define QSPI_TXFIFO        (QSPI_BASE + 0x108)
#define QSPI_RXFIFO        (QSPI_BASE + 0x188)

typedef struct tegra_qspi {
  spi_t spi;
  mutex_t mutex;
  uint32_t cmd;
} tegra_qspi_t;


static void
qspi_lock(spi_t *dev, int acquire)
{
  tegra_qspi_t *qspi = (tegra_qspi_t *)dev;
  if(acquire)
    mutex_lock(&qspi->mutex);
  else
    mutex_unlock(&qspi->mutex);
}


static void
qspi_xfer_init(tegra_qspi_t *qspi)
{
  qspi->cmd &= ~(1 << 20);
  reg_wr(QSPI_COMMAND, qspi->cmd);
}


static void
qspi_xfer_fini(tegra_qspi_t *qspi)
{
  qspi->cmd |= (1 << 20);
  reg_wr(QSPI_COMMAND, qspi->cmd);
}


static error_t
qspi_cmd(tegra_qspi_t *qspi, uint32_t cmd)
{
  reg_wr(QSPI_COMMAND, cmd);

  int cnt = 0;
  while(1) {
    uint32_t r = reg_rd(QSPI_STATUS);
    cnt++;
    if(r & (1 << 30)) {
      reg_wr(QSPI_STATUS, r);
      break;
    }
  }
  return 0;
}

static error_t
qspi_xfer(tegra_qspi_t *qspi, const uint8_t *tx, uint8_t *rx, size_t len)
{
  if(!tx == !rx || len == 0) // QSPI does not support concurrent TX & RX
    return ERR_INVALID_PARAMETER;

  const int packed = !(len & 3);
  const size_t max_chunk = packed ? 256 : 64;
  const uint32_t cmd = qspi->cmd | (1 << 31) | (packed ? (1 << 5) : 0);
  error_t err;

  if(tx) {

    while(len) {
      size_t chunk = MIN(len, max_chunk);

      if(packed) {
        for(size_t i = 0; i < chunk / 4; i++) {
          uint32_t word = tx[0] | (tx[1] << 8) | (tx[2] << 16) | (tx[3] << 24);
          reg_wr(QSPI_TXFIFO, word);
          tx += 4;
        }

      } else {
        for(size_t i = 0; i < chunk; i++) {
          reg_wr(QSPI_TXFIFO, *tx++);
        }
      }

      reg_wr(QSPI_BLK_SIZE, chunk - 1);
      err = qspi_cmd(qspi, cmd | (1 << 11));
      if(err)
        return err;
      len -= chunk;
    }

  } else {

    while(len) {
      size_t chunk = MIN(len, max_chunk);

      reg_wr(QSPI_BLK_SIZE, chunk - 1);
      err = qspi_cmd(qspi, cmd | (1 << 12));
      if(err)
        return err;

      if(packed) {
        for(size_t i = 0; i < chunk / 4; i++) {
          uint32_t word = reg_rd(QSPI_RXFIFO);
          *rx++ = word;
          *rx++ = word >> 8;
          *rx++ = word >> 16;
          *rx++ = word >> 24;
        }
      } else {
        for(size_t i = 0; i < chunk; i++) {
          *rx++ = reg_rd(QSPI_RXFIFO);
        }
      }

      len -= chunk;
    }
  }

  return 0;
}


static error_t
qspi_rw_locked(spi_t *dev, const uint8_t *tx, uint8_t *rx, size_t len,
               gpio_t nss, int config)
{
  tegra_qspi_t *qspi = (tegra_qspi_t *)dev;
  qspi_xfer_init(qspi);
  error_t err = qspi_xfer(qspi, tx, rx, len);
  qspi_xfer_fini(qspi);
  return err;
}


static error_t
qspi_rw(spi_t *dev, const uint8_t *tx, uint8_t *rx, size_t len, gpio_t nss,
        int config)
{
  tegra_qspi_t *qspi = (tegra_qspi_t *)dev;
  mutex_lock(&qspi->mutex);
  error_t err = qspi_rw_locked(&qspi->spi, tx, rx, len, nss, config);
  mutex_unlock(&qspi->mutex);
  return err;
}


static error_t
qspi_rwv(struct spi *dev, const struct iovec *txiov,
         const struct iovec *rxiov, size_t count,
         gpio_t nss, int config)
{
  tegra_qspi_t *qspi = (tegra_qspi_t *)dev;
  error_t err = 0;
  mutex_lock(&qspi->mutex);

  qspi_xfer_init(qspi);

  for(size_t i = 0; i < count; i++) {
    err = qspi_xfer(qspi, txiov[i].iov_base,
                    rxiov ? rxiov[i].iov_base : NULL, txiov[i].iov_len);
    if(err)
      break;
  }
  qspi_xfer_fini(qspi);
  mutex_unlock(&qspi->mutex);
  return err;
}


static int
qspi_get_config(spi_t *dev, int clock_flags, int baudrate)
{
  return 0;
}


spi_t *
tegra_qspi_init(void)
{
  clk_enable(194);
  reset_peripheral(76);

  tegra_qspi_t *qspi = calloc(1, sizeof(tegra_qspi_t));

  mutex_init(&qspi->mutex, "qspi");

  qspi->cmd = 0x40300007;

  qspi->spi.rw = qspi_rw;
  qspi->spi.rwv = qspi_rwv;
  qspi->spi.rw_locked = qspi_rw_locked;
  qspi->spi.lock = qspi_lock;
  qspi->spi.get_config = qspi_get_config;

  reg_wr(QSPI_COMMAND, qspi->cmd);

  return &qspi->spi;
}
