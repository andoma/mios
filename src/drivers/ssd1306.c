#include "ssd1306.h"

#include <mios/io.h>
#include <mios/task.h>
#include <stdlib.h>
#include <sys/uio.h>
#include <sys/param.h>
#include <string.h>

#include "ssd1306_font.h"

struct ssd1306 {
  i2c_t *bus;
  uint8_t buf[2];
  uint8_t addr;
  mutex_t mutex;
};

static const uint8_t init_commands[] = {
   0xae, 0xd5, 0x80, 0xa8, 0x3f, 0xd3, 0x00, 0x40, 0x8d, 0x14, 0x20, 0x01,
   0xa1, 0xc8, 0xda, 0x12, 0x81, 0xcf, 0xd9, 0xf1, 0xdb, 0x40, 0xa4, 0xa6
};

ssd1306_t *
ssd1306_create(i2c_t *bus)
{
  ssd1306_t *d = malloc(sizeof(ssd1306_t));
  d->bus = bus;
  d->addr = 0x3c;
  mutex_init(&d->mutex, "ssd1306");
  return d;
}

static error_t
send_command(ssd1306_t *dev, uint8_t cmd)
{
  dev->buf[0] = 0x80;
  dev->buf[1] = cmd;
  return i2c_rw(dev->bus, dev->addr, dev->buf, sizeof(dev->buf), NULL, 0);
}

static error_t
ssd1306_init_locked(ssd1306_t *dev)
{
  error_t err;
  for(size_t i = 0; i < sizeof(init_commands); i++) {
    err = send_command(dev, init_commands[i]);
    if(err)
      return err;
  }

  send_command(dev, 0x21);
  send_command(dev, 0);
  send_command(dev, 127);

  send_command(dev, 0x22);
  send_command(dev, 0);
  send_command(dev, 7);

  dev->buf[0] = 0x40;
  int length = 1024;
  while(length) {
    int chunk = MIN(length, 128);
    struct iovec tx[2] = {{&dev->buf, 1}, {NULL, chunk}};
    err = dev->bus->rwv(dev->bus, dev->addr, tx, NULL, 2);
    if(err)
      return err;
    length -= chunk;
  }
  return send_command(dev, 0xaf); // Enable display
}

error_t
ssd1306_init(ssd1306_t *dev)
{
  mutex_lock(&dev->mutex);
  error_t err = ssd1306_init_locked(dev);
  mutex_unlock(&dev->mutex);
  return err;
}


static int
get_width(int cp)
{
  if(cp > 127)
    return 0;

  if(cp < 33) {
    return 6;
  }

  cp -= 33;

  return fontoffset[cp + 1] - fontoffset[cp];
}

static error_t
draw_char(ssd1306_t *dev, int col, int cp)
{
  if(cp > 127)
    return 0;

  send_command(dev, 0x21);
  send_command(dev, col);
  send_command(dev, 127);

  int w = 0;
  const uint8_t *src;
  if(cp < 33) {
    src = NULL;
    w = 6;
  } else {
    cp -= 33;

    int offset = fontoffset[cp];
    w = fontoffset[cp + 1] - offset;
    src = &fontbitmap[offset * 2];
  }
  dev->buf[0] = 0x40;
  struct iovec tx[2] = {{&dev->buf, 1}, {(void *)src, w * 2}};

  return dev->bus->rwv(dev->bus, dev->addr, tx, NULL, 2);
}


static error_t
draw_space(ssd1306_t *dev, int start, int length)
{
  send_command(dev, 0x21);
  send_command(dev, start);
  send_command(dev, 127);

  dev->buf[0] = 0x40;

  while(length > 0) {
    int cols = MIN(length, 32);
    struct iovec tx[2] = {{&dev->buf, 1}, {NULL, cols * 2}};
    error_t err = dev->bus->rwv(dev->bus, dev->addr, tx, NULL, 2);
    if(err)
      return err;
    length -= cols;
  }
  return 0;
}



static error_t
ssd1306_print_locked(ssd1306_t *dev, int row, const char *str)
{
  send_command(dev, 0x22);
  send_command(dev, row * 2);
  send_command(dev, row * 2 + 1);

  size_t len = strlen(str);

  int total_width = 0;
  for(size_t i = 0; i < len; i++) {
    total_width += i ? 2 : 0;
    total_width += get_width(str[i]);
  }

  if(total_width > 127)
    total_width = 127;

  int col = 64 - total_width / 2;

  error_t err;
  err = draw_space(dev, 0, col + 1);
  if(err)
    return err;

  for(size_t i = 0; i < len; i++) {
    if(i) {
      err = draw_space(dev, col, 2);
      if(err)
        return err;
      col += 2;
    }
    err = draw_char(dev, col, str[i]);
    if(err)
      return err;
    col += get_width(str[i]);
  }

  if(col < 127) {
    err = draw_space(dev, col, 127 - col + 1);
    if(err)
      return err;
  }
  return 0;
}

error_t
ssd1306_print(ssd1306_t *dev, int row, const char *str)
{
  if(row < 0 || row > 3)
    return ERR_INVALID_ARGS;
  mutex_lock(&dev->mutex);
  error_t err = ssd1306_print_locked(dev, row, str);
  mutex_unlock(&dev->mutex);
  return err;
}
