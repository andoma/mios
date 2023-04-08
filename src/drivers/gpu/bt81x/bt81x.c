#include "bt81x.h"

#include <assert.h>
#include <mios/mios.h>
#include <mios/task.h>
#include <mios/gfx.h>
#include <mios/eventlog.h>
#include <sys/uio.h>
#include <sys/param.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <math.h>

#include "bt81x_def.h"
#include "irq.h"

static const gfx_display_class_t bt81x_ops;


typedef struct bitmap_header {
  uint16_t format;
  uint16_t width;
  uint16_t height;
  uint8_t data[0];
} bitmap_header_t;





typedef struct bt81x {

  gfx_display_t gfx_display;

  const bt81x_timings_t *timings;

  uint8_t irq;
  uint8_t enabled;
  uint8_t running;
  uint8_t backlight;
  task_waitable_t irq_waitq;

  // IO
  spi_t *spi;
  gpio_t gpio_ncs;
  gpio_t gpio_pd;
  gpio_t gpio_irq;
  uint32_t spi_cfg;

  uint16_t cmd_write;

  int dlptr;
  uint8_t pad0;
  uint8_t dladdr0;
  uint8_t dladdr1;
  uint8_t dladdr2;
  uint32_t dl[2048];

  gfx_rect_t display_size;

  const bt81x_bitmap_t *bitmaps;
  size_t num_bitmaps;

  uint32_t bitmap_addr[0];
} bt81x_t;


static uint8_t
bt81x_rd8(bt81x_t *b, uint32_t addr)
{
  uint8_t tx[5] = {addr >> 16, addr >> 8, addr, 0, 0};
  uint8_t rx[5];
  error_t err = b->spi->rw(b->spi, tx, rx, sizeof(tx), b->gpio_ncs, b->spi_cfg);
  if(err)
    printf("%s: %x err:%d\n", __FUNCTION__, addr, err);

  return rx[4];
}


static uint32_t
bt81x_rd16(bt81x_t *b, uint32_t addr)
{
  uint8_t tx[6] = {addr >> 16, addr >> 8, addr};
  uint8_t rx[6];
  error_t err = b->spi->rw(b->spi, tx, rx, sizeof(tx), b->gpio_ncs, b->spi_cfg);
  if(err)
    printf("%s: %x err:%d\n", __FUNCTION__, addr, err);
  return rx[4] | (rx[5] << 8);
}

static uint32_t
bt81x_rd32(bt81x_t *b, uint32_t addr)
{
  uint8_t tx[8] = {addr >> 16, addr >> 8, addr};
  uint8_t rx[8];
  error_t err = b->spi->rw(b->spi, tx, rx, sizeof(tx), b->gpio_ncs, b->spi_cfg);
  if(err)
    printf("%s: %x err:%d\n", __FUNCTION__, addr, err);
  return rx[4] | (rx[5] << 8) | (rx[6] << 16) | (rx[7] << 24);
}



static void
bt81x_cmd(bt81x_t *b, uint8_t command, uint8_t param)
{
  uint8_t tx[3] = {command, param, 0};
  error_t err = b->spi->rw(b->spi, tx, NULL, sizeof(tx), b->gpio_ncs, b->spi_cfg);
  if(err)
    printf("%s: %x err:%d\n", __FUNCTION__, command, err);
}


static void
bt81x_wr8(bt81x_t *b, uint32_t addr, uint8_t value)
{
  uint8_t tx[4] = {0x80 | (addr >> 16), addr >> 8, addr,
                       value};
  error_t err = b->spi->rw(b->spi, tx, NULL, sizeof(tx), b->gpio_ncs, b->spi_cfg);
  if(err)
    printf("%s: %x=%x err:%d\n", __FUNCTION__, addr, value, err);
}

static void
bt81x_wr16(bt81x_t *b, uint32_t addr, uint32_t value)
{
  uint8_t tx[5] = {0x80 | (addr >> 16), addr >> 8, addr,
                       value, value >> 8};
  error_t err = b->spi->rw(b->spi, tx, NULL, sizeof(tx), b->gpio_ncs, b->spi_cfg);
  if(err)
    printf("%s: %x err:%d\n", __FUNCTION__, addr, err);
}

static void
bt81x_wr32(bt81x_t *b, uint32_t addr, uint32_t value)
{
  uint8_t tx[7] = {0x80 | (addr >> 16), addr >> 8, addr,
                       value, value >> 8, value >> 16, value >> 24};
  error_t err = b->spi->rw(b->spi, tx, NULL, sizeof(tx), b->gpio_ncs, b->spi_cfg);
  if(err)
    printf("%s: %x err:%d\n", __FUNCTION__, addr, err);
}


static error_t
bt81x_reset(bt81x_t *b)
{
  b->cmd_write = 0;
  gpio_set_output(b->gpio_pd, 0);
  usleep(20000);
  gpio_set_output(b->gpio_pd, 1);
  usleep(20000);

  const int clock_mul = 6;  // 12MHz * 6 = 72MHz

  bt81x_cmd(b, EVE_CMD_CLKEXT, 0);
  bt81x_cmd(b, EVE_CMD_CLKSEL, 0x4 | clock_mul);
  bt81x_cmd(b, EVE_CMD_ACTIVE, 0);

  usleep(20000);

  int retry = 0;

  while(bt81x_rd8(b, EVE_REG_ID) != 0x7c) {
    retry++;
    if(retry == 10)
      return ERR_INVALID_ID;
    usleep(10000);
  }

  retry = 0;
  while(bt81x_rd8(b, EVE_REG_CPURESET) != 0) {
    retry++;
    if(retry == 10)
      return ERR_NO_DEVICE;
    usleep(10000);
  }

  bt81x_wr32(b, EVE_REG_FREQUENCY, 12000000 * clock_mul);
  return 0;
}

struct bt81x_font {
  uint8_t bitmap_handle;
  uint8_t height;
  uint8_t baseline;
  uint8_t width[96];
};


static const struct bt81x_font fonts[] = {
  {
    .bitmap_handle = 17,
    .height = 108,
    .baseline = 22,
    .width = {
      23, 25, 33, 57, 54, 68, 57, 20, 31, 31, 40, 52, 20, 41, 24, 38,
      52, 52, 52, 52, 52, 52, 52, 52, 52, 52, 23, 23, 46, 52, 48, 44,
      82, 58, 58, 58, 63, 50, 50, 62, 65, 26, 50, 58, 51, 79, 65, 63,
      58, 64, 58, 56, 56, 62, 58, 79, 58, 58, 55, 25, 38, 25, 38, 43,
      29, 50, 52, 48, 52, 48, 31, 52, 52, 23, 23, 47, 23, 80, 52, 52,
      51, 52, 32, 48, 29, 52, 46, 70, 46, 46, 46, 31, 23, 31, 63, 23,
    }
  },{
    .bitmap_handle = 19,
    .height = 83,
    .baseline = 17,
    .width = {
      18, 19, 25, 44, 41, 52, 44, 15, 24, 24, 31, 41, 16, 32, 19, 29,
      40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 18, 18, 36, 40, 37, 34,
      63, 45, 45, 45, 48, 39, 39, 48, 50, 20, 40, 45, 39, 62, 50, 49,
      45, 50, 45, 43, 42, 48, 45, 61, 45, 45, 42, 19, 29, 19, 30, 34,
      22, 39, 40, 37, 40, 37, 25, 41, 41, 18, 18, 36, 18, 63, 41, 40,
      40, 40, 25, 38, 23, 41, 36, 54, 36, 36, 36, 24, 18, 24, 47, 18,
    }
  },{
    .bitmap_handle = 20,
    .height = 63,
    .baseline = 12,
    .width = {
      13, 15, 19, 33, 31, 40, 34, 11, 18, 18, 24, 30, 12, 24, 14, 22,
      30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 13, 14, 28, 30, 29, 26,
      49, 34, 34, 34, 36, 29, 29, 37, 37, 15, 30, 34, 29, 46, 37, 37,
      34, 38, 33, 33, 32, 37, 34, 46, 34, 34, 32, 15, 22, 15, 23, 26,
      17, 30, 31, 28, 31, 29, 19, 31, 31, 13, 14, 28, 13, 47, 31, 31,
      31, 31, 19, 29, 17, 31, 27, 41, 27, 27, 27, 18, 14, 18, 36, 13,
    }
  },{
    .bitmap_handle = 31,
    .height = 49,
    .baseline = 10,
    .width = {
      10, 11, 15, 26, 25, 31, 26, 10, 15, 14, 18, 24, 9, 18, 11, 17,
      24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 10, 10, 21, 23, 22, 20,
      37, 27, 27, 26, 28, 23, 22, 28, 29, 12, 23, 26, 22, 35, 29, 28,
      26, 29, 27, 26, 26, 28, 27, 36, 27, 26, 25, 12, 18, 12, 18, 21,
      13, 23, 24, 22, 24, 22, 15, 24, 24, 10, 11, 22, 10, 36, 24, 24,
      24, 24, 15, 22, 14, 24, 21, 32, 21, 21, 22, 15, 10, 15, 29, 10,
    }
  },{
    .bitmap_handle = 30,
    .height = 36,
    .baseline = 7,
    .width = {
      8, 9, 12, 19, 18, 23, 19, 7, 11, 10, 14, 17, 7, 15, 8, 13,
      17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 7, 8, 16, 18, 16, 15,
      28, 20, 19, 20, 22, 16, 17, 22, 23, 9, 17, 19, 17, 26, 23, 22,
      19, 22, 19, 20, 19, 21, 20, 27, 20, 19, 18, 9, 13, 9, 13, 16,
      10, 17, 17, 16, 17, 16, 12, 18, 17, 7, 8, 16, 7, 27, 17, 17,
      17, 17, 11, 17, 11, 17, 16, 23, 16, 16, 15, 11, 7, 10, 21, 8,
    }
  },{
    .bitmap_handle = 29,
    .height = 28,
    .baseline = 5,
    .width = {
      6, 6, 8, 15, 15, 17, 15, 5, 9, 8, 11, 14, 5, 11, 7, 10,
      14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 6, 6, 12, 14, 13, 12,
      21, 15, 15, 15, 17, 13, 13, 16, 17, 7, 13, 16, 13, 21, 17, 16,
      15, 17, 15, 14, 15, 17, 15, 21, 15, 15, 14, 7, 10, 7, 10, 13,
      8, 13, 14, 12, 14, 12, 10, 14, 14, 6, 6, 13, 6, 21, 14, 13,
      14, 13, 9, 12, 9, 14, 12, 18, 12, 12, 12, 8, 6, 9, 15, 6,
    }
  }
};


__attribute__((unused))
static void
dump_font_table(bt81x_t *b)
{
  uint32_t fta = bt81x_rd32(b, ROMFONT_TABLEADDRESS);
  printf("font table @ %x\n", fta);
  for(int i = 0; i < 19; i++) {
    uint32_t fmt = bt81x_rd32(b, fta + 128);
    uint32_t fls = bt81x_rd32(b, fta + 132);
    uint32_t w   = bt81x_rd32(b, fta + 136);
    uint32_t h   = bt81x_rd32(b, fta + 140);
    uint32_t ptr = bt81x_rd32(b, fta + 144);

    printf("%2d @ %08x | %08x %3d %3d %3d 0x%08x 0x%08x\n",
           16 + i, fta, fmt, fls, w, h, ptr, ptr - 32 * fls * h);

    for(int i = 0; i < 128; i++) {
      printf("%d, ", bt81x_rd8(b, fta + i));
      if((i & 15) == 15)
        printf("\n");
    }
    fta += 148;
  }
}


static error_t
bt81x_cop_writev(bt81x_t *b, const void *data, size_t len)
{
  uint8_t hdr[3] = {0x80 | 0x30};

  uint8_t zeropad[3] = {0};
  struct iovec iov[3];

  iov[0].iov_base = hdr;
  iov[0].iov_len = 3;
  iov[2].iov_base = zeropad;

  while(len) {
    const uint16_t cmd_read = bt81x_rd32(b, EVE_REG_CMD_READ);
    const uint16_t fullness = (b->cmd_write - cmd_read) & 0xfff;
    const uint16_t free_space = (4096 - 4) - fullness;

    const size_t to_send = MIN(free_space, len);

    if(to_send == 0) {
      panic("fifo full");
    }

    int pad = (to_send & 3) ? (4 - (to_send & 3)) : 0;

    iov[1].iov_base = (void *)data;
    iov[1].iov_len = len;
    iov[2].iov_len = pad;
    hdr[1] = 0x80 | (b->cmd_write >> 8);
    hdr[2] = b->cmd_write;

    error_t err = b->spi->txv(b->spi, iov, 2 + !!pad, b->gpio_ncs, b->spi_cfg);
    if(err)
      return err;
    b->cmd_write = (b->cmd_write + len + pad) & 0xfff;
    bt81x_wr32(b, EVE_REG_CMD_WRITE, (b->cmd_write - 4) & 0xfff);
    len -= to_send;
    data += to_send;
  }
  return 0;
}


static uint32_t
bt81x_bitmap_linesize(const bitmap_header_t *bh)
{
  switch(bh->format) {
  case EVE_FORMAT_L4:
    return bh->width >> 1;
  default:
    panic("%s: Unsupported format %d\n", __FUNCTION__, bh->format);
  }
}


static uint32_t
bitmap_uncompressed_size(const bitmap_header_t *bh)
{
  return bt81x_bitmap_linesize(bh) * bh->height;
}


static error_t
bt81x_upload_images(bt81x_t *b)
{
  uint32_t addr = 0;
  error_t err;

  const bt81x_bitmap_t *bb = b->bitmaps;
  for(size_t i = 0; i < b->num_bitmaps; i++) {
    const uint32_t inflate_cmd[2] = {EVE_ENC_CMD_INFLATE, addr};
    b->bitmap_addr[i] = addr;

    err = bt81x_cop_writev(b, inflate_cmd, sizeof(inflate_cmd));
    if(err)
      return err;

    const bitmap_header_t *bh = bb->data;

    err = bt81x_cop_writev(b, bh->data, bb->size - sizeof(bitmap_header_t));
    if(err)
      return err;

    uint32_t ainc = bitmap_uncompressed_size(bh);
    addr += ainc;
    bb++;
  }

  return 0;
}




static void
bt81x_swap(bt81x_t *b)
{
  error_t err = b->spi->rw(b->spi, &b->dladdr0, NULL, 3 + b->dlptr * 4,
                           b->gpio_ncs, b->spi_cfg);
  if(err)
    printf("Swap failed: %d\n", err);
  bt81x_wr32(b, EVE_REG_DLSWAP, EVE_DLSWAP_FRAME);
}



static const uint32_t setup_displaylist[] = {
  // Font 34 at handle 17
  EVE_ENC_BITMAP_HANDLE(17),
  EVE_ENC_BITMAP_SOURCE(0x1fe330),
  EVE_ENC_BITMAP_LAYOUT_H(0,0),
  EVE_ENC_BITMAP_LAYOUT(31,128,14),
  EVE_ENC_BITMAP_SWIZZLE(1,1,1,2),
  EVE_ENC_BITMAP_EXT_FORMAT(0x93ba),
  EVE_ENC_BITMAP_SIZE_H(0,0),
  EVE_ENC_BITMAP_SIZE(0,0,0,78,108),
  // Font 33 at handle 19
  EVE_ENC_BITMAP_HANDLE(19),
  EVE_ENC_BITMAP_SOURCE(0x22b330),
  EVE_ENC_BITMAP_LAYOUT_H(0,0),
  EVE_ENC_BITMAP_LAYOUT(31,128,11),
  EVE_ENC_BITMAP_SWIZZLE(1,1,1,2),
  EVE_ENC_BITMAP_EXT_FORMAT(0x93b7),
  EVE_ENC_BITMAP_SIZE_H(0,0),
  EVE_ENC_BITMAP_SIZE(0,0,0,60,83),
  // Font 32 at handle 20
  EVE_ENC_BITMAP_HANDLE(20),
  EVE_ENC_BITMAP_SOURCE(0x251330),
  EVE_ENC_BITMAP_LAYOUT_H(0,0),
  EVE_ENC_BITMAP_LAYOUT(31,96,8),
  EVE_ENC_BITMAP_SWIZZLE(1,1,1,2),
  EVE_ENC_BITMAP_EXT_FORMAT(0x93b7),
  EVE_ENC_BITMAP_SIZE_H(0,0),
  EVE_ENC_BITMAP_SIZE(0,0,0,46,63),

  EVE_ENC_CLEAR_COLOR_RGB(0,0,0),
  EVE_ENC_CLEAR(1,1,1),
  EVE_ENC_DISPLAY()
};


static error_t
load_initial_displaylist(bt81x_t *b)
{
  struct iovec v[2];
  v[0].iov_base = &b->dladdr0;
  v[0].iov_len = 3;
  v[1].iov_base = (void *)setup_displaylist;
  v[1].iov_len = sizeof(setup_displaylist);

  error_t err = b->spi->txv(b->spi, v, 2, b->gpio_ncs, b->spi_cfg);
  if(err)
    return err;
  bt81x_wr32(b, EVE_REG_DLSWAP, EVE_DLSWAP_FRAME);
  return 0;
}


static error_t
bt81x_initialize(bt81x_t *b)
{
  error_t err = bt81x_reset(b);
  if(err) {
    evlog(LOG_ERR, "bt81x: Failed to initialize, err=%d", err);
    return err;
  }

  evlog(LOG_DEBUG, "bt81x: Initialized");

  // Disable pixel clock
  bt81x_wr8(b, EVE_REG_PCLK, 0);

  // Turn off backlight
  bt81x_wr16(b, EVE_REG_PWM_DUTY, 0);


  // Initialize scanout
  const bt81x_timings_t *t = b->timings;
  bt81x_wr16(b, EVE_REG_HSIZE,   t->width);
  bt81x_wr16(b, EVE_REG_HCYCLE,  t->hcycle);
  bt81x_wr16(b, EVE_REG_HOFFSET, t->hoffset);
  bt81x_wr16(b, EVE_REG_HSYNC0,  t->hsync0);
  bt81x_wr16(b, EVE_REG_HSYNC1,  t->hsync1);
  bt81x_wr16(b, EVE_REG_VSIZE,   t->height);
  bt81x_wr16(b, EVE_REG_VCYCLE,  t->vcycle);
  bt81x_wr16(b, EVE_REG_VOFFSET, t->voffset);
  bt81x_wr16(b, EVE_REG_VSYNC0,  t->vsync0);
  bt81x_wr16(b, EVE_REG_VSYNC1,  t->vsync1);
  bt81x_wr8(b, EVE_REG_SWIZZLE,  t->swizzle);
  bt81x_wr8(b, EVE_REG_PCLK_POL, t->pclk_pol);

  // FIXME: Add drivestrength?

  // Disable adaptive frameate
  bt81x_wr16(b, EVE_REG_ADAPTIVE_FRAMERATE, 0);

  bt81x_wr8(b, EVE_REG_TOUCH_MODE, 3);
  bt81x_wr8(b, EVE_REG_CTOUCH_EXTENDED, EVE_CTOUCH_MODE_COMPATIBILITY);

  // Clear pending interrupts
  bt81x_rd32(b, EVE_REG_INT_FLAGS);

  err = load_initial_displaylist(b);
  if(err)
    return err;

  // Enable backlight and pixelclock
  bt81x_wr16(b, EVE_REG_GPIOX, bt81x_rd16(b, EVE_REG_GPIOX) | 0x8000);

  bt81x_wr8(b, EVE_REG_PCLK, 2);

  // Enable interrupts
  bt81x_wr16(b, EVE_REG_INT_MASK, 0x81);
  bt81x_wr16(b, EVE_REG_INT_EN,   0x1);

  bt81x_upload_images(b);

  bt81x_wr16(b, EVE_REG_PWM_HZ, 250);
  bt81x_wr16(b, EVE_REG_PWM_DUTY, b->backlight);

  return 0;
}



static void
draw_gui(bt81x_t *b)
{
  gfx_display_t *gd = &b->gfx_display;
  gd->gd_tab_offsets_size = 0;
  b->dlptr = 0;

  b->dl[b->dlptr++] = EVE_ENC_RESTORE_CONTEXT(); // XXX???

  b->dl[b->dlptr++] = EVE_ENC_CLEAR_COLOR_RGB(0,0,0) | gd->gd_palette[0];

  b->dl[b->dlptr++] = 0x4000000 | (gd->gd_palette[1] & 0xffffff);
  b->dl[b->dlptr++] = EVE_ENC_COLOR_A(255);

  b->dl[b->dlptr++] = EVE_ENC_CLEAR(1, 1, 1);

  pthread_mutex_lock(&gd->gd_mutex);

  gd->gd_gdd->gdd_draw(gd->gd_opaque, gd, &b->display_size);

  pthread_mutex_unlock(&gd->gd_mutex);
  b->dl[b->dlptr++] = EVE_ENC_DISPLAY();
  bt81x_swap(b);
}


static void
touch_conversion_completed(bt81x_t *b)
{
  gfx_display_t *gd = &b->gfx_display;

  const uint32_t xy = bt81x_rd32(b, EVE_REG_TOUCH_RAW_XY);

  pthread_mutex_lock(&gd->gd_mutex);

  if(xy == 0xffffffff) {
    if(gd->gd_gdd->gdd_touch_release != NULL)
      gd->gd_gdd->gdd_touch_release(gd->gd_opaque, gd);
  } else {
    const gfx_position_t p = {xy >> 16, xy & 0xffff};
    if(gd->gd_gdd->gdd_touch_press != NULL)
      gd->gd_gdd->gdd_touch_press(gd->gd_opaque, gd, &p);
  }
  pthread_mutex_unlock(&gd->gd_mutex);
}



__attribute__((noreturn))
static void *
bt81x_thread(void *arg)
{
  bt81x_t *b = arg;

  while(1) {

    if(!b->enabled) {
      bt81x_wr16(b, EVE_REG_PWM_DUTY, 0);
      usleep(1000);
      gpio_set_output(b->gpio_pd, 0);
      task_sleep(&b->irq_waitq);
      continue;
    }

    error_t err = bt81x_initialize(b);
    if(err) {
      if(task_sleep_delta(&b->irq_waitq, 1000000)) {}
      continue;
    }
    int q = irq_forbid(IRQ_LEVEL_CLOCK);
    b->running = 1;
    while(b->enabled) {

      if(!b->irq) {
        if(task_sleep_delta(&b->irq_waitq, 100000)) {
          evlog(LOG_WARNING, "bt81x: timeout");
          break;
        }
        continue;
      }

      const uint32_t intflags = bt81x_rd32(b, EVE_REG_INT_FLAGS);
      if(intflags == 0xffffffff)
        break;

      irq_permit(q);

      if(intflags & 0x1) {
        draw_gui(b);
      }

      if(intflags & 0x80) {
        touch_conversion_completed(b);
      }

      q = irq_forbid(IRQ_LEVEL_CLOCK);
    }
    b->running = 0;
    irq_permit(q);
  }
}





static void
bt81x_irq(void *arg)
{
  bt81x_t *b = arg;
  b->irq = !gpio_get_input(b->gpio_irq); // line is active low, so we invert
  if(b->irq)
    task_wakeup(&b->irq_waitq, 0); // If asserted, wakeup thread
}



static bt81x_t *g_bt81x;

struct gfx_display *
bt81x_create(spi_t *spi, gpio_t ncs, gpio_t pd, gpio_t irq,
             const bt81x_timings_t *timings,
             const bt81x_bitmap_t bitmaps[],
             size_t num_bitmaps,
             const gfx_display_delegate_t *gdd,
             void *opaque,
             int enabled)
{
  bt81x_t *b = calloc(1, sizeof(bt81x_t) + sizeof(uint32_t) * num_bitmaps);
  g_bt81x = b;

  task_waitable_init(&b->irq_waitq, "gpu");

  b->display_size.siz.width = timings->width;
  b->display_size.siz.height = timings->height;
  b->enabled = enabled;
  b->backlight = 64;
  b->timings = timings;
  b->spi = spi;
  b->gpio_ncs = ncs;
  b->gpio_pd = pd;
  b->gpio_irq = irq;
  b->spi_cfg = spi->get_config(spi, 0, 16000000);

  b->dladdr0 = 0x80 | 0x30;

  b->bitmaps = bitmaps;
  b->num_bitmaps = num_bitmaps;

  gpio_conf_output(b->gpio_ncs, GPIO_PUSH_PULL, GPIO_SPEED_LOW, GPIO_PULL_NONE);
  gpio_set_output(b->gpio_ncs, 1);
  gpio_conf_output(b->gpio_pd, GPIO_PUSH_PULL, GPIO_SPEED_LOW, GPIO_PULL_NONE);

  gpio_conf_irq(b->gpio_irq, GPIO_PULL_UP, bt81x_irq, b,
                GPIO_FALLING_EDGE | GPIO_RISING_EDGE,
                IRQ_LEVEL_CLOCK);

  b->gfx_display.gd_class = &bt81x_ops;
  b->gfx_display.gd_gdd = gdd;
  b->gfx_display.gd_opaque = opaque;

  pthread_mutex_init(&b->gfx_display.gd_mutex, NULL);
  b->gfx_display.gd_palette[1] = 0xffffff;

  thread_create(bt81x_thread, b, 1024, "gpu", TASK_FPU, 3);
  return &b->gfx_display;
}


void
bt81x_enable(gfx_display_t *gd, int enabled)
{
  bt81x_t *b = (bt81x_t *)gd;
  int q = irq_forbid(IRQ_LEVEL_CLOCK);
  b->enabled = enabled;
  task_wakeup(&b->irq_waitq, 0);
  irq_permit(q);
}


void
bt81x_backlight(gfx_display_t *gd, uint8_t backlight)
{
  bt81x_t *b = (bt81x_t *)gd;
  b->backlight = backlight;
  if(b->running)
    bt81x_wr16(b, EVE_REG_PWM_DUTY, b->backlight);
}


/************************************************************************
 *
 * GUI draw primitives
 *
 */

static int
ensure_display_list(bt81x_t *b, int need)
{
  if(b->dlptr + need > 2040) {
    return 0;
  }
  return 1;
}


static void
push_state(gfx_display_t *gd)
{
  bt81x_t *b = (bt81x_t *)gd;

  if(!ensure_display_list(b, 1))
    return;
  b->dl[b->dlptr++] = EVE_ENC_SAVE_CONTEXT();
}


static void
pop_state(gfx_display_t *gd)
{
  bt81x_t *b = (bt81x_t *)gd;

  if(!ensure_display_list(b, 1))
    return;
  b->dl[b->dlptr++] = EVE_ENC_RESTORE_CONTEXT();
}


static void
scissor(gfx_display_t *gd, const gfx_rect_t *r)
{
  bt81x_t *b = (bt81x_t *)gd;

  if(!ensure_display_list(b, 2))
    return;
  b->dl[b->dlptr++] = EVE_ENC_SCISSOR_XY(r->pos.x, r->pos.y);
  b->dl[b->dlptr++] = EVE_ENC_SCISSOR_SIZE(r->siz.width, r->siz.height);

}


static void
draw_primitive(gfx_display_t *gd,
               int x1, int y1, int x2, int y2,
               int line_width, int which)

{
  bt81x_t *b = (bt81x_t *)gd;

  if(!ensure_display_list(b, 4))
    return;

  b->dl[b->dlptr++] = EVE_ENC_BEGIN(which);
  b->dl[b->dlptr++] = EVE_ENC_LINE_WIDTH(line_width);

  b->dl[b->dlptr++] = EVE_ENC_VERTEX2F(x1 << 4, y1 << 4);
  b->dl[b->dlptr++] = EVE_ENC_VERTEX2F(x2 << 4, y2 << 4);
  b->dl[b->dlptr++] = EVE_ENC_END();
}

static void
draw_line(gfx_display_t *gd,
          int x1, int y1, int x2, int y2,
          int line_width)
{
  draw_primitive(gd, x1, y1, x2, y2, line_width << 4,
                 EVE_BEGIN_LINES);
}

static void
draw_filled_rect(gfx_display_t *gd,
                 const gfx_rect_t *r,
                 int corner_radius)
{
  const int x1 = r->pos.x;
  const int x2 = r->pos.x + r->siz.width;
  const int y1 = r->pos.y;
  const int y2 = r->pos.y + r->siz.height;

  draw_primitive(gd, x1, y1, x2, y2, corner_radius << 4,
                 EVE_BEGIN_RECTS);
}


static void
draw_rect(gfx_display_t *gd,
                 const gfx_rect_t *r,
                 int line_width)
{
  bt81x_t *b = (bt81x_t *)gd;
  if(!ensure_display_list(b, 7))
    return;

  const int x1 = r->pos.x;
  const int x2 = r->pos.x + r->siz.width;
  const int y1 = r->pos.y;
  const int y2 = r->pos.y + r->siz.height;

  b->dl[b->dlptr++] = EVE_ENC_BEGIN(EVE_BEGIN_LINE_STRIP);
  b->dl[b->dlptr++] = EVE_ENC_LINE_WIDTH(line_width << 4);

  b->dl[b->dlptr++] = EVE_ENC_VERTEX2F(x1 << 4, y1 << 4);
  b->dl[b->dlptr++] = EVE_ENC_VERTEX2F(x2 << 4, y1 << 4);
  b->dl[b->dlptr++] = EVE_ENC_VERTEX2F(x2 << 4, y2 << 4);
  b->dl[b->dlptr++] = EVE_ENC_VERTEX2F(x1 << 4, y2 << 4);
  b->dl[b->dlptr++] = EVE_ENC_VERTEX2F(x1 << 4, y1 << 4);
  b->dl[b->dlptr++] = EVE_ENC_END();
}

static void
draw_text(gfx_display_t *gd,
          const gfx_position_t *pos,
          gfx_font_id_t font,
          const char *str, size_t len)
{
  bt81x_t *b = (bt81x_t *)gd;

  if(!ensure_display_list(b, 2 + 4 + len + 1))
    return;

  const struct bt81x_font *f = &fonts[font];

  b->dl[b->dlptr++] = EVE_ENC_SAVE_CONTEXT();
  b->dl[b->dlptr++] = EVE_ENC_VERTEX_TRANSLATE_X(pos->x << 4);
  b->dl[b->dlptr++] = EVE_ENC_VERTEX_TRANSLATE_Y(pos->y << 4);
  b->dl[b->dlptr++] = EVE_ENC_BEGIN(EVE_BEGIN_BITMAPS);

  int handle = f->bitmap_handle;

  int x = 0;
  int absx = 0;
  size_t ti = 0;
  for(size_t i = 0; i < len; i++) {
    uint8_t c = str[i];
    if(c == '\t' && ti < gd->gd_tab_offsets_size) {
      x = gd->gd_tab_offsets[ti++] - absx;
    } else if(c >= 32 && c < 128) {
      if(x > 512) {
        b->dl[b->dlptr++] = EVE_ENC_VERTEX_TRANSLATE_X((x + pos->x) << 4);
        absx = x;
        x = 0;
      }

      b->dl[b->dlptr++] = EVE_ENC_VERTEX2II(x, 0, handle, c);
      x += f->width[c - 32];
    }
  }
  b->dl[b->dlptr++] = EVE_ENC_END();
  b->dl[b->dlptr++] = EVE_ENC_RESTORE_CONTEXT();
}


static void
draw_bitmap(gfx_display_t *gd,
            const gfx_position_t *pos,
            int bitmap)
{
  bt81x_t *b = (bt81x_t *)gd;

  if(!ensure_display_list(b, 3 + 3 + 3 + 1))
    return;

  b->dl[b->dlptr++] = EVE_ENC_SAVE_CONTEXT();
  b->dl[b->dlptr++] = EVE_ENC_VERTEX_TRANSLATE_X(pos->x << 4);
  b->dl[b->dlptr++] = EVE_ENC_VERTEX_TRANSLATE_Y(pos->y << 4);

  assert(bitmap < b->num_bitmaps);

  const bt81x_bitmap_t *bb = &b->bitmaps[bitmap];
  const bitmap_header_t *bh = bb->data;

  int linesize = bt81x_bitmap_linesize(bh);

  b->dl[b->dlptr++] = EVE_ENC_BITMAP_LAYOUT(2, linesize, bh->height);
  b->dl[b->dlptr++] = EVE_ENC_BITMAP_SIZE(0, 0, 0, bh->width, bh->height);
  b->dl[b->dlptr++] = EVE_ENC_BITMAP_SOURCE(b->bitmap_addr[bitmap]);

  b->dl[b->dlptr++] = EVE_ENC_BEGIN(EVE_BEGIN_BITMAPS);
  b->dl[b->dlptr++] = EVE_ENC_VERTEX2II(0, 0, 0, 0);
  b->dl[b->dlptr++] = EVE_ENC_END();

  b->dl[b->dlptr++] = EVE_ENC_RESTORE_CONTEXT();
}


static void
set_color(gfx_display_t *gd, uint32_t rgb)
{
  bt81x_t *b = (bt81x_t *)gd;

  if(!ensure_display_list(b, 1))
    return;
  if(rgb & 0x80000000) {
    rgb = gd->gd_palette[rgb & (GFX_DISPLAY_PALETTE_SIZE - 1)];
  }
  b->dl[b->dlptr++] = 0x4000000 | (rgb & 0xffffff);
}


static void
set_alpha(gfx_display_t *gd, uint8_t alpha)
{
  bt81x_t *b = (bt81x_t *)gd;

  if(!ensure_display_list(b, 1))
    return;
  b->dl[b->dlptr++] = EVE_ENC_COLOR_A(alpha);
}


static gfx_size_t
get_text_size(gfx_display_t *gd,
              gfx_font_id_t font,
              const char *str, size_t len)
{
  const struct bt81x_font *f = &fonts[font];
  int x = 0;
  size_t ti = 0;
  for(size_t i = 0; i < len; i++) {
    uint8_t c = str[i];
    if(c == '\t' && ti < gd->gd_tab_offsets_size) {
      x = gd->gd_tab_offsets[ti++];
    } else if(c >= 32 && c < 128) {
      x += f->width[c - 32];
    }
  }
  return (gfx_size_t){x, f->height};
}


static int
get_font_baseline(gfx_display_t *gd, gfx_font_id_t font)
{
  const struct bt81x_font *f = &fonts[font];
  return f->height - f->baseline;
}

static gfx_size_t
get_bitmap_size(gfx_display_t *gd, int bitmap)
{
  bt81x_t *b = (bt81x_t *)gd;
  assert(bitmap < b->num_bitmaps);
  const bt81x_bitmap_t *bb = &b->bitmaps[bitmap];
  const bitmap_header_t *bh = bb->data;
  return (gfx_size_t){bh->width, bh->height};
}


static const gfx_display_class_t bt81x_ops = {
  .push_state = push_state,
  .pop_state = pop_state,
  .scissor = scissor,
  .line = draw_line,
  .rect = draw_rect,
  .filled_rect = draw_filled_rect,
  .text = draw_text,
  .bitmap = draw_bitmap,
  .set_color = set_color,
  .set_alpha = set_alpha,
  .get_text_size = get_text_size,
  .get_font_baseline = get_font_baseline,
  .get_bitmap_size = get_bitmap_size,
};

#if 1

/************************************************************************
 *
 * MISC debug support
 *
 */

#include <mios/cli.h> // XXX remove

#include <string.h>

static error_t
cmd_eve_rd32(cli_t *cli, int argc, char **argv)
{
  if(g_bt81x == NULL)
    return -1;

  if(argc < 2) {
    cli_printf(cli, "eve_rd32 <start> [count]\n");
    return -1;
  }

  int start;

  if(!strcmp(argv[1], "dl")) {
    start = 0x300000;
  } else if(!strcmp(argv[1], "cmd")) {
    start = 0x308000;
  } else {
    start = atoix(argv[1]);
  }
  const int count = argc > 2 ? atoix(argv[2]) : 1;

  for(int i = 0; i < count; i++) {
    cli_printf(cli, "0x%08x: 0x%08x\n",
               start, bt81x_rd32(g_bt81x, start));
    start += 4;
  }
  return 0;
}

CLI_CMD_DEF("eve_rd32", cmd_eve_rd32);


static error_t
cmd_font_table(cli_t *cli, int argc, char **argv)
{
  if(g_bt81x == NULL)
    return -1;

  bt81x_t *b = g_bt81x;

  dump_font_table(b);
  return 0;
}

CLI_CMD_DEF("font-table", cmd_font_table);



static error_t
cmd_cop(cli_t *cli, int argc, char **argv)
{
  if(g_bt81x == NULL)
    return -1;

  bt81x_t *b = g_bt81x;

  cli_printf(cli, " READ=%x\n", bt81x_rd32(b, EVE_REG_CMD_READ));
  cli_printf(cli, "WRITE=%x\n", bt81x_rd32(b, EVE_REG_CMD_WRITE));

  return 0;
}

CLI_CMD_DEF("cop", cmd_cop);





static error_t
cmd_gpu_info(cli_t *cli, int argc, char **argv)
{
  if(g_bt81x == NULL)
    return -1;

  bt81x_t *b = g_bt81x;

  uint32_t intflags = bt81x_rd32(b, EVE_REG_INT_FLAGS);
  cli_printf(cli, "gpio: %d\n", !gpio_get_input(b->gpio_irq));
  cli_printf(cli, "b->irq=%d\n", b->irq);
  cli_printf(cli, "intflags=0x%x\n", intflags);
  return 0;
}

CLI_CMD_DEF("gpu-info", cmd_gpu_info);
#endif
