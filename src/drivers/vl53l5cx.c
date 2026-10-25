#include "vl53l5cx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <malloc.h>
#include <sys/param.h>

#include <mios/io.h>
#include <mios/type_macros.h>
#include <mios/task.h>
#include <mios/eventlog.h>

#include "vl53l5cx_fw.h"

// Register level sequences from ST's VL53L5CX ULD driver (BSD-3-Clause,
// Copyright 2021 STMicroelectronics). The firmware exchanges data in big
// endian 32 bit words, see swap32().

#define UI_CMD_STATUS  0x2c00
#define UI_CMD_START   0x2c04
#define UI_CMD_END     0x2fff

#define NVM_DATA_SIZE    492
#define OFFSET_SIZE      488
#define XTALK_SIZE       776
#define TMP_SIZE         1024

// DCI (device configuration interface) indexes
#define DCI_ZONE_CONFIG     0x5450
#define DCI_FREQ_HZ         0x5458
#define DCI_PIPE_CONTROL    0xcf78
#define DCI_RANGING_MODE    0xad30
#define DCI_DSS_CONFIG      0xad38
#define DCI_SINGLE_RANGE    0xcd5c
#define DCI_OUTPUT_CONFIG   0xcd60
#define DCI_OUTPUT_ENABLES  0xcd68
#define DCI_OUTPUT_LIST     0xcd78

// Result block headers: type (bits 0-3), size (4-15), index (16-31)
#define BH_START           0x0000000d
#define BH_METADATA        0x54b400c0
#define BH_COMMONDATA      0x54c00040
#define BH_AMBIENT_RATE    0x54d00104
#define BH_SPAD_COUNT      0x55d00404
#define BH_NB_TARGETS      0xcf7c0401
#define BH_SIGNAL_RATE     0xcfbc0404
#define BH_RANGE_SIGMA     0xd2bc0402
#define BH_DISTANCE        0xd33c0402
#define BH_REFLECTANCE     0xd43c0401
#define BH_TARGET_STATUS   0xd47c0401
#define BH_MOTION_DETECT   0xcc5008c0

#define IDX_AMBIENT_RATE   0x54d0
#define IDX_NB_TARGETS     0xcf7c
#define IDX_SIGNAL_RATE    0xcfbc
#define IDX_RANGE_SIGMA    0xd2bc
#define IDX_DISTANCE       0xd33c
#define IDX_TARGET_STATUS  0xd47c

static const uint32_t output_list[] = {
  BH_START, BH_METADATA, BH_COMMONDATA, BH_AMBIENT_RATE, BH_SPAD_COUNT,
  BH_NB_TARGETS, BH_SIGNAL_RATE, BH_RANGE_SIGMA, BH_DISTANCE,
  BH_REFLECTANCE, BH_TARGET_STATUS, BH_MOTION_DETECT,
};

// Mandatory blocks (start, metadata, common) plus ambient, number of
// targets, signal, sigma, distance and target status
#define OUTPUT_ENABLE \
  ((1 << 0) | (1 << 1) | (1 << 2) | (1 << 3) | (1 << 5) | (1 << 6) | \
   (1 << 7) | (1 << 8) | (1 << 10))

struct vl53l5cx {
  i2c_t *bus;
  mutex_t mutex;
  uint8_t zones;
  uint8_t ranging;
  const char *step; // For error messages
  uint8_t stream_count;
  uint16_t read_size;
  uint8_t offset[OFFSET_SIZE];
  uint8_t xtalk[XTALK_SIZE];
  uint32_t tmp32[TMP_SIZE / 4];
};

#define TMP(v) ((uint8_t *)(v)->tmp32)


static void
swap32(uint8_t *buf, size_t size)
{
  for(size_t i = 0; i < size; i += 4) {
    uint32_t w;
    memcpy(&w, buf + i, 4);
    w = __builtin_bswap32(w);
    memcpy(buf + i, &w, 4);
  }
}


static uint32_t
ld32(const uint8_t *p, int i)
{
  uint32_t v;
  memcpy(&v, p + i * 4, 4);
  return v;
}

static void
st32(uint8_t *p, int i, uint32_t v)
{
  memcpy(p + i * 4, &v, 4);
}

static int16_t
ld16(const uint8_t *p, int i)
{
  int16_t v;
  memcpy(&v, p + i * 2, 2);
  return v;
}

static void
st16(uint8_t *p, int i, int16_t v)
{
  memcpy(p + i * 2, &v, 2);
}


//
// I2C access. Registers have 16 bit addresses and auto increment.
// The I2C driver moves at most 255 bytes per transfer.
//

#define I2C_CHUNK 252

static error_t
wr(vl53l5cx_t *v, uint16_t reg, const uint8_t *data, size_t len)
{
  while(len) {
    const size_t n = MIN(len, I2C_CHUNK);
    uint8_t addr[2] = { reg >> 8, reg };
    const struct iovec tx[2] = {
      { addr, 2 }, { (void *)data, n }
    };
    error_t err = v->bus->rwv(v->bus, VL53L5CX_I2C_ADDR, tx, NULL, 2);
    if(err)
      return err;
    reg += n;
    data += n;
    len -= n;
  }
  return 0;
}


static error_t
rd(vl53l5cx_t *v, uint16_t reg, uint8_t *data, size_t len)
{
  while(len) {
    const size_t n = MIN(len, I2C_CHUNK);
    uint8_t addr[2] = { reg >> 8, reg };
    error_t err = i2c_rw(v->bus, VL53L5CX_I2C_ADDR, addr, 2, data, n);
    if(err)
      return err;
    reg += n;
    data += n;
    len -= n;
  }
  return 0;
}


static error_t
wrb(vl53l5cx_t *v, uint16_t reg, uint8_t val)
{
  return wr(v, reg, &val, 1);
}


static error_t
rdb(vl53l5cx_t *v, uint16_t reg, uint8_t *val)
{
  return rd(v, reg, val, 1);
}


typedef struct {
  uint16_t reg;
  uint8_t val;
} regval_t;

static error_t
wr_table(vl53l5cx_t *v, const regval_t *t, size_t count)
{
  for(size_t i = 0; i < count; i++) {
    error_t err = wrb(v, t[i].reg, t[i].val);
    if(err)
      return err;
  }
  return 0;
}

#define WR_TABLE(v, t) wr_table(v, t, sizeof(t) / sizeof(t[0]))


// Poll until (buf[pos] & mask) == expected, 10 ms interval, 2 s timeout
static error_t
poll_for_answer(vl53l5cx_t *v, size_t size, int pos, uint16_t reg,
                uint8_t mask, uint8_t expected)
{
  uint8_t *buf = TMP(v);
  for(int i = 0; i < 200; i++) {
    error_t err = rd(v, reg, buf, size);
    if(err)
      return err;
    if((buf[pos] & mask) == expected)
      return 0;
    if(size >= 4 && buf[2] >= 0x7f) {
      evlog(LOG_ERR, "vl53l5cx: MCU error during %s, "
            "0x%04x: %02x %02x %02x %02x after %d ms",
            v->step, reg, buf[0], buf[1], buf[2], buf[3], i * 10);
      return ERR_OPERATION_FAILED;
    }
    usleep(10000);
  }
  evlog(LOG_ERR, "vl53l5cx: Timeout during %s, 0x%04x: %02x",
        v->step, reg, buf[pos]);
  return ERR_TIMEOUT;
}


static error_t
wait_cmd(vl53l5cx_t *v)
{
  return poll_for_answer(v, 4, 1, UI_CMD_STATUS, 0xff, 0x03);
}


//
// DCI read / write. Sizes are multiples of 4 and at most 20 bytes here.
//

static error_t
dci_read(vl53l5cx_t *v, uint16_t index, uint8_t *data, size_t size)
{
  const uint8_t cmd[12] = {
    index >> 8, index, (size & 0xff0) >> 4, (size & 0xf) << 4,
    0x00, 0x00, 0x00, 0x0f, 0x00, 0x02, 0x00, 0x08
  };
  error_t err;
  if((err = wr(v, UI_CMD_END - 11, cmd, sizeof(cmd))) ||
     (err = wait_cmd(v)) ||
     (err = rd(v, UI_CMD_START, TMP(v), size + 12)))
    return err;
  swap32(TMP(v), size + 12);
  memcpy(data, TMP(v) + 4, size);
  return 0;
}


static error_t
dci_write(vl53l5cx_t *v, uint16_t index, const void *data, size_t size)
{
  uint8_t *buf = TMP(v);
  buf[0] = index >> 8;
  buf[1] = index;
  buf[2] = (size & 0xff0) >> 4;
  buf[3] = (size & 0xf) << 4;
  memcpy(buf + 4, data, size);
  swap32(buf + 4, size);
  const uint8_t footer[8] = {
    0x00, 0x00, 0x00, 0x0f, 0x05, 0x01, (size + 8) >> 8, (size + 8)
  };
  memcpy(buf + 4 + size, footer, sizeof(footer));

  error_t err = wr(v, UI_CMD_END - (size + 12) + 1, buf, size + 12);
  if(err)
    return err;
  return wait_cmd(v);
}


static error_t
dci_replace(vl53l5cx_t *v, uint16_t index, size_t size,
            const void *data, size_t len, size_t pos)
{
  uint8_t buf[20];
  error_t err = dci_read(v, index, buf, size);
  if(err)
    return err;
  memcpy(buf + pos, data, len);
  return dci_write(v, index, buf, size);
}


//
// Calibration data. 4x4 mode needs the 8x8 grids averaged down.
//

static void
average_4x4_u32(uint8_t *grid)
{
  // In place is fine: zone i + 4j is written after it has been read and
  // before any of its sources (2i + 16j + {0, 1, 8, 9}) are overwritten
  for(int j = 0; j < 4; j++) {
    for(int i = 0; i < 4; i++) {
      const int s = 2 * i + 16 * j;
      st32(grid, i + 4 * j, (ld32(grid, s) + ld32(grid, s + 1) +
                             ld32(grid, s + 8) + ld32(grid, s + 9)) / 4);
    }
  }
  memset(grid + 16 * 4, 0, 48 * 4);
}


static error_t
send_offset_data(vl53l5cx_t *v, int zones)
{
  uint8_t *buf = TMP(v);
  memcpy(buf, v->offset, OFFSET_SIZE);

  if(zones == 16) {
    static const uint8_t dss_4x4[8] = {
      0x0f, 0x04, 0x04, 0x00, 0x08, 0x10, 0x10, 0x07
    };
    memcpy(buf + 0x10, dss_4x4, sizeof(dss_4x4));
    swap32(buf, OFFSET_SIZE);
    average_4x4_u32(buf + 0x3c);

    uint8_t *range = buf + 0x140;
    for(int j = 0; j < 4; j++) {
      for(int i = 0; i < 4; i++) {
        const int s = 2 * i + 16 * j;
        st16(range, i + 4 * j, (ld16(range, s) + ld16(range, s + 1) +
                                ld16(range, s + 8) + ld16(range, s + 9)) / 4);
      }
    }
    memset(range + 16 * 2, 0, 48 * 2);
    swap32(buf, OFFSET_SIZE);
  }

  memmove(buf, buf + 8, OFFSET_SIZE - 8);
  static const uint8_t footer[8] = {
    0x00, 0x00, 0x00, 0x0f, 0x03, 0x01, 0x01, 0xe4
  };
  memcpy(buf + 0x1e0, footer, sizeof(footer));

  error_t err = wr(v, 0x2e18, buf, OFFSET_SIZE);
  if(err)
    return err;
  return wait_cmd(v);
}


static error_t
send_xtalk_data(vl53l5cx_t *v, int zones)
{
  uint8_t *buf = TMP(v);
  memcpy(buf, v->xtalk, XTALK_SIZE);

  if(zones == 16) {
    static const uint8_t res_4x4[8] = {
      0x0f, 0x04, 0x04, 0x17, 0x08, 0x10, 0x10, 0x07
    };
    static const uint8_t dss_4x4[8] = {
      0x00, 0x78, 0x00, 0x08, 0x00, 0x00, 0x00, 0x08
    };
    static const uint8_t profile_4x4[4] = { 0xa0, 0xfc, 0x01, 0x00 };
    memcpy(buf + 0x08, res_4x4, sizeof(res_4x4));
    memcpy(buf + 0x20, dss_4x4, sizeof(dss_4x4));
    swap32(buf, XTALK_SIZE);
    average_4x4_u32(buf + 0x34);
    swap32(buf, XTALK_SIZE);
    memcpy(buf + 0x134, profile_4x4, sizeof(profile_4x4));
    memset(buf + 0x78, 0, 4);
  }

  error_t err = wr(v, 0x2cf8, buf, XTALK_SIZE);
  if(err)
    return err;
  return wait_cmd(v);
}


//
// Boot and firmware upload
//

static const regval_t sw_reboot_1[] = {
  {0x7fff, 0x00}, {0x0009, 0x04}, {0x000f, 0x40}, {0x000a, 0x03},
};

static const regval_t sw_reboot_2[] = {
  {0x000c, 0x01}, {0x0101, 0x00}, {0x0102, 0x00}, {0x010a, 0x01},
  {0x4002, 0x01}, {0x4002, 0x00}, {0x010a, 0x03}, {0x0103, 0x01},
  {0x000c, 0x00}, {0x000f, 0x43},
};

// Preceded by host access to GO1
static const regval_t power_on[] = {
  {0x0101, 0x00}, {0x0102, 0x00}, {0x010a, 0x01}, {0x4002, 0x01},
  {0x4002, 0x00}, {0x010a, 0x03}, {0x0103, 0x01}, {0x400f, 0x00},
  {0x021a, 0x43}, {0x021a, 0x03}, {0x021a, 0x01}, {0x021a, 0x00},
  {0x0219, 0x00}, {0x021b, 0x00},
  {0x7fff, 0x00}, {0x0c, 0x00},           // Wake up MCU
  {0x7fff, 0x01}, {0x20, 0x07}, {0x20, 0x06},
};

static const regval_t reset_mcu[] = {
  {0x7fff, 0x00}, {0x0114, 0x00}, {0x0115, 0x00}, {0x0116, 0x42},
  {0x0117, 0x00}, {0x000b, 0x00},
};

static const regval_t boot_mcu[] = {
  {0x000c, 0x00}, {0x000b, 0x01},
};


// GO2 status 0 (0x06) bit 7 = MCU booted, then status 1 (0x07) bit 0 =
// firmware ready. Polling for 0x06 == 0 (as ST's ULD 1.x does) can pass
// before the firmware is up. This is the check from ST's later ULD.
static error_t
poll_for_mcu_boot(vl53l5cx_t *v)
{
  for(int i = 0; i < 500; i++) {
    uint8_t s0, s1;
    error_t err = rdb(v, 0x06, &s0);
    if(err)
      return err;
    if(s0 & 0x80) {
      if((err = rdb(v, 0x07, &s1)))
        return err;
      if(s1 & 0x01)
        return 0;
    }
    if(s0 & 0x01) {
      // ST's code stops polling here without flagging an error
      evlog(LOG_WARNING, "vl53l5cx: GO2 status 0x%02x during MCU boot", s0);
      return 0;
    }
    usleep(1000);
  }
  evlog(LOG_ERR, "vl53l5cx: Timeout waiting for MCU boot");
  return ERR_TIMEOUT;
}


static error_t
enable_fw_access(vl53l5cx_t *v)
{
  error_t err;
  if((err = wrb(v, 0x7fff, 0x02)) ||
     (err = wrb(v, 0x03, 0x0d)) ||
     (err = wrb(v, 0x7fff, 0x01)) ||
     (err = poll_for_answer(v, 1, 0, 0x21, 0x10, 0x10)) ||
     (err = wrb(v, 0x7fff, 0x00)))
    return err;
  return 0;
}


static error_t
vl53l5cx_boot(vl53l5cx_t *v)
{
  error_t err;
  uint8_t tmp;

  v->step = "reboot";
  if((err = WR_TABLE(v, sw_reboot_1)) ||
     (err = rdb(v, 0x7fff, &tmp)) ||
     (err = WR_TABLE(v, sw_reboot_2)))
    return err;
  usleep(1000);
  if((err = wrb(v, 0x000f, 0x40)) ||
     (err = wrb(v, 0x000a, 0x01)))
    return err;
  usleep(100000);

  // Wait for the boot ROM
  if((err = wrb(v, 0x7fff, 0x00)) ||
     (err = poll_for_answer(v, 1, 0, 0x06, 0xff, 1)) ||
     (err = wrb(v, 0x000e, 0x01)) ||
     (err = enable_fw_access(v)) ||
     (err = rdb(v, 0x7fff, &tmp)) ||
     (err = wrb(v, 0x0c, 0x01)) ||
     (err = WR_TABLE(v, power_on)))
    return err;

  // Firmware download, three pages of RAM
  v->step = "firmware download";
  for(int i = 0; i < 3; i++) {
    const size_t off = i * 0x8000;
    const size_t len = MIN(sizeof(vl53l5cx_firmware) - off, 0x8000);
    if((err = wrb(v, 0x7fff, 0x09 + i)) ||
       (err = wr(v, 0, vl53l5cx_firmware + off, len)))
      return err;
  }

  if((err = wrb(v, 0x7fff, 0x01)) ||
     (err = enable_fw_access(v)) ||
     (err = wrb(v, 0x0c, 0x01)) ||
     (err = WR_TABLE(v, reset_mcu)) ||
     (err = rdb(v, 0x7fff, &tmp)) ||
     (err = WR_TABLE(v, boot_mcu)))
    return err;
  v->step = "MCU boot";
  if((err = poll_for_mcu_boot(v)) ||
     (err = wrb(v, 0x7fff, 0x02)))
    return err;

  // Read the offset calibration from NVM
  v->step = "NVM read";
  if((err = wr(v, 0x2fd8, vl53l5cx_get_nvm_cmd,
               sizeof(vl53l5cx_get_nvm_cmd))) ||
     (err = poll_for_answer(v, 4, 0, UI_CMD_STATUS, 0xff, 2)) ||
     (err = rd(v, UI_CMD_START, TMP(v), NVM_DATA_SIZE)))
    return err;
  memcpy(v->offset, TMP(v), OFFSET_SIZE);
  memcpy(v->xtalk, vl53l5cx_default_xtalk, XTALK_SIZE);

  v->step = "offset data";
  if((err = send_offset_data(v, 16)))
    return err;
  v->step = "xtalk data";
  if((err = send_xtalk_data(v, 16)))
    return err;
  v->step = "default config";
  if((err = wr(v, 0x2c34, vl53l5cx_default_config,
               sizeof(vl53l5cx_default_config))) ||
     (err = wait_cmd(v)))
    return err;
  v->step = "pipe control";

  // One target per zone
  const uint8_t pipe_ctrl[4] = { 1, 0x00, 0x01, 0x00 };
  const uint32_t single_range = 1;
  if((err = dci_write(v, DCI_PIPE_CONTROL, pipe_ctrl, sizeof(pipe_ctrl))) ||
     (err = dci_write(v, DCI_SINGLE_RANGE, &single_range, 4)))
    return err;

  v->zones = 16;
  v->step = "operation";
  return 0;
}


vl53l5cx_t *
vl53l5cx_create(i2c_t *bus)
{
  vl53l5cx_t *v = xalloc(sizeof(vl53l5cx_t), 0, MEM_MAY_FAIL);
  if(v == NULL)
    return NULL;
  memset(v, 0, sizeof(vl53l5cx_t));
  v->bus = bus;
  mutex_init(&v->mutex, "vl53l5cx");

  uint8_t id = 0, rev = 0;
  error_t err;
  if((err = wrb(v, 0x7fff, 0x00)) ||
     (err = rdb(v, 0, &id)) ||
     (err = rdb(v, 1, &rev)) ||
     (err = wrb(v, 0x7fff, 0x02)) ||
     id != 0xf0 || rev != 0x02) {
    evlog(LOG_ERR, "vl53l5cx: Not detected (err:%d id:0x%02x rev:0x%02x)",
          err, id, rev);
    free(v);
    return NULL;
  }

  const int64_t t0 = clock_get();
  if((err = vl53l5cx_boot(v))) {
    evlog(LOG_ERR, "vl53l5cx: Init failed: %d", err);
    free(v);
    return NULL;
  }
  evlog(LOG_INFO, "vl53l5cx: Firmware loaded in %d ms",
        (int)((clock_get() - t0) / 1000));
  return v;
}


//
// Ranging
//

static error_t
set_resolution(vl53l5cx_t *v, int zones)
{
  uint8_t buf[16];
  error_t err;
  const int side = zones == 64 ? 8 : 4;

  if((err = dci_read(v, DCI_DSS_CONFIG, buf, 16)))
    return err;
  buf[0x04] = zones == 64 ? 16 : 64;
  buf[0x06] = buf[0x04];
  buf[0x09] = zones == 64 ? 1 : 4;
  if((err = dci_write(v, DCI_DSS_CONFIG, buf, 16)) ||
     (err = dci_read(v, DCI_ZONE_CONFIG, buf, 8)))
    return err;
  buf[0x00] = side;
  buf[0x01] = side;
  buf[0x04] = 32 / side;
  buf[0x05] = 32 / side;
  if((err = dci_write(v, DCI_ZONE_CONFIG, buf, 8)) ||
     (err = send_offset_data(v, zones)) ||
     (err = send_xtalk_data(v, zones)))
    return err;
  v->zones = zones;
  return 0;
}


static error_t
set_continuous(vl53l5cx_t *v)
{
  uint8_t buf[8];
  error_t err;
  if((err = dci_read(v, DCI_RANGING_MODE, buf, 8)))
    return err;
  buf[0x01] = 0x1;
  buf[0x03] = 0x3;
  const uint32_t single_range = 0;
  if((err = dci_write(v, DCI_RANGING_MODE, buf, 8)) ||
     (err = dci_write(v, DCI_SINGLE_RANGE, &single_range, 4)))
    return err;
  return 0;
}


static error_t
start_ranging(vl53l5cx_t *v)
{
  uint32_t list[ARRAYSIZE(output_list)];
  const uint32_t enables[4] = { OUTPUT_ENABLE, 0, 0, 0xc0000000 };
  size_t size = 0;

  memcpy(list, output_list, sizeof(list));

  // Size of the result buffer, per-zone blocks get the zone count
  for(size_t i = 0; i < ARRAYSIZE(list); i++) {
    if(!(enables[i / 32] & (1 << (i % 32))))
      continue;
    const uint32_t type = list[i] & 0xf;
    uint32_t bsize = (list[i] >> 4) & 0xfff;
    if(type >= 0x1 && type < 0xd) {
      bsize = v->zones;
      list[i] = (list[i] & 0xffff000f) | (bsize << 4);
      size += type * bsize;
    } else {
      size += bsize;
    }
    size += 4;
  }
  size += 20;
  if(size > TMP_SIZE)
    return ERR_NO_BUFFER;
  v->read_size = size;

  const uint32_t header_config[2] = { size, ARRAYSIZE(list) + 1 };
  error_t err;
  if((err = dci_write(v, DCI_OUTPUT_LIST, list, sizeof(list))) ||
     (err = dci_write(v, DCI_OUTPUT_CONFIG, header_config, 8)) ||
     (err = dci_write(v, DCI_OUTPUT_ENABLES, enables, sizeof(enables))))
    return err;

  // Start xshut bypass (interrupt mode)
  if((err = wrb(v, 0x7fff, 0x00)) ||
     (err = wrb(v, 0x09, 0x05)) ||
     (err = wrb(v, 0x7fff, 0x02)))
    return err;

  const uint8_t cmd[4] = { 0x00, 0x03, 0x00, 0x00 };
  if((err = wr(v, UI_CMD_END - 3, cmd, sizeof(cmd))) ||
     (err = wait_cmd(v)))
    return err;

  v->stream_count = 255;
  v->ranging = 1;
  return 0;
}


static error_t
stop_ranging(vl53l5cx_t *v)
{
  uint8_t flag[4];
  error_t err;

  v->ranging = 0;
  if((err = rd(v, 0x2ffc, flag, 4)))
    return err;

  if(ld32(flag, 0) != 0x4ff) {
    // Provoke MCU stop and wait for it
    if((err = wrb(v, 0x7fff, 0x00)) ||
       (err = wrb(v, 0x15, 0x16)) ||
       (err = wrb(v, 0x14, 0x01)))
      return err;
    int i;
    for(i = 0; i < 500; i++) {
      uint8_t s;
      if((err = rdb(v, 0x06, &s)))
        return err;
      if(s & 0x80)
        break;
      usleep(10000);
    }
    if(i == 500)
      err = ERR_TIMEOUT;
  }

  static const regval_t undo_stop[] = {
    {0x7fff, 0x00}, {0x14, 0x00}, {0x15, 0x00},
    {0x09, 0x04},                           // Stop xshut bypass
    {0x7fff, 0x02},
  };
  error_t err2 = WR_TABLE(v, undo_stop);
  return err ?: err2;
}


error_t
vl53l5cx_start(vl53l5cx_t *v, unsigned int zones_per_side, unsigned int hz)
{
  if(zones_per_side != 4 && zones_per_side != 8)
    return ERR_INVALID_ARGS;
  const int zones = zones_per_side * zones_per_side;
  if(hz < 1 || hz > (zones == 64 ? 15 : 60))
    return ERR_INVALID_ARGS;

  mutex_lock(&v->mutex);
  error_t err = 0;
  if(v->ranging)
    err = stop_ranging(v);
  const uint8_t freq = hz;
  if(!err && v->zones != zones)
    err = set_resolution(v, zones);
  if(!err)
    err = dci_replace(v, DCI_FREQ_HZ, 4, &freq, 1, 0x01);
  if(!err)
    err = set_continuous(v);
  if(!err)
    err = start_ranging(v);
  mutex_unlock(&v->mutex);
  return err;
}


error_t
vl53l5cx_stop(vl53l5cx_t *v)
{
  mutex_lock(&v->mutex);
  error_t err = v->ranging ? stop_ranging(v) : 0;
  mutex_unlock(&v->mutex);
  return err;
}


int
vl53l5cx_is_ranging(vl53l5cx_t *v)
{
  return v->ranging;
}


static error_t
check_data_ready(vl53l5cx_t *v)
{
  uint8_t s[4];
  error_t err = rd(v, 0, s, 4);
  if(err)
    return err;
  if(s[0] != v->stream_count && s[0] != 255 && s[1] == 0x5 &&
     (s[2] & 0x5) == 0x5 && (s[3] & 0x10) == 0x10)
    return 0;
  return ERR_NOT_READY;
}


static void
decode(vl53l5cx_t *v, vl53l5cx_frame_t *f)
{
  uint8_t *buf = TMP(v);
  const size_t size = v->read_size;
  const int zones = v->zones;
  uint8_t nb_targets[VL53L5CX_MAX_ZONES];

  memset(f, 0, sizeof(vl53l5cx_frame_t));
  memset(nb_targets, 0, sizeof(nb_targets));
  f->zones = zones;
  f->stream_count = buf[0];

  swap32(buf, size);

  // Skip the headers, then walk the blocks
  for(size_t i = 16; i + 4 <= size; i += 4) {
    const uint32_t bh = ld32(buf, i / 4);
    const uint32_t type = bh & 0xf;
    const uint32_t bsize = (bh >> 4) & 0xfff;
    const uint32_t idx = bh >> 16;
    const size_t msize = type > 0x1 && type < 0xd ? type * bsize : bsize;
    const uint8_t *src = buf + i + 4;
    if(i + 4 + msize > size)
      break;

    switch(idx) {
    case IDX_AMBIENT_RATE:
      for(int z = 0; z < zones; z++)
        f->ambient[z] = ld32(src, z) / 2048;
      break;
    case IDX_NB_TARGETS:
      memcpy(nb_targets, src, zones);
      break;
    case IDX_SIGNAL_RATE:
      for(int z = 0; z < zones; z++)
        f->signal[z] = ld32(src, z) / 2048;
      break;
    case IDX_RANGE_SIGMA:
      for(int z = 0; z < zones; z++)
        f->sigma_mm[z] = (uint16_t)ld16(src, z) / 128;
      break;
    case IDX_DISTANCE:
      for(int z = 0; z < zones; z++) {
        const int d = ld16(src, z) / 4;
        f->distance_mm[z] = d < 0 ? 0 : d;
      }
      break;
    case IDX_TARGET_STATUS:
      memcpy(f->status, src, zones);
      break;
    }
    i += msize;
  }

  for(int z = 0; z < zones; z++) {
    if(nb_targets[z] == 0)
      f->status[z] = 255;
  }
}


error_t
vl53l5cx_read(vl53l5cx_t *v, vl53l5cx_frame_t *f, int64_t deadline)
{
  while(1) {
    mutex_lock(&v->mutex);
    if(!v->ranging) {
      mutex_unlock(&v->mutex);
      return ERR_NOT_READY;
    }
    error_t err = check_data_ready(v);
    if(err == 0) {
      err = rd(v, 0, TMP(v), v->read_size);
      if(err == 0) {
        v->stream_count = TMP(v)[0];
        decode(v, f);
      }
      mutex_unlock(&v->mutex);
      return err;
    }
    mutex_unlock(&v->mutex);
    if(err != ERR_NOT_READY)
      return err;
    if(clock_get() > deadline)
      return ERR_TIMEOUT;
    usleep(5000);
  }
}


void
vl53l5cx_print_frame(const vl53l5cx_frame_t *f, struct stream *st)
{
  const int side = f->zones == 64 ? 8 : 4;
  stprintf(st, "Frame %d, distance in mm (- = no target, ? = low confidence)\n",
           f->stream_count);
  for(int y = 0; y < side; y++) {
    for(int x = 0; x < side; x++) {
      const int z = y * side + x;
      if(f->status[z] == 255)
        stprintf(st, "     -");
      else
        stprintf(st, " %4d%c", f->distance_mm[z],
                 vl53l5cx_zone_valid(f, z) ? ' ' : '?');
    }
    stprintf(st, "\n");
  }
}
