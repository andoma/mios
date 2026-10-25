// ST TCPP03 USB Type-C port protection and power path controller.
//
// Own implementation, register layout and switching sequences follow
// ST's tcpp0203 component driver and STM32N6570-DK BSP (BSD-3-Clause).
//
// A single write-only control register (REG0) and two read registers:
// REG1 echoes the control register (with some fields encoded
// differently) and REG2 holds the fault and status flags.

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

#include <mios/device.h>
#include <mios/stream.h>

#include "tcpp03.h"

#define REG_CTRL   0x00
#define REG_ACK    0x01
#define REG_FLAGS  0x02

// REG0
#define CTRL_GDP_CLOSED     (1 << 2) // Gate driver provider (source path)
#define CTRL_GDC_OPEN       (1 << 3) // Gate driver consumer (sink path)
#define CTRL_MODE_HIBERNATE (0 << 4)
#define CTRL_MODE_NORMAL    (1 << 4)
#define CTRL_MODE_LOWPOWER  (2 << 4)
#define CTRL_MODE_MASK      (3 << 4)
#define CTRL_VBUS_DISCHARGE (1 << 6)

// REG2
#define FLAG_OCP_VCONN  (1 << 0)
#define FLAG_OCP_VBUS   (1 << 1)
#define FLAG_OVP_VBUS   (1 << 2)
#define FLAG_OVP_CC     (1 << 3)
#define FLAG_OTP        (1 << 4)
#define FLAG_VBUS_OK    (1 << 5)
#define FLAG_TCPP02     (1 << 7)

struct tcpp03 {
  device_t t_dev;
  i2c_t *t_i2c;
  uint8_t t_address;
  uint8_t t_ctrl;   // Last value written to REG0
  error_t t_err;    // Last I2C error
};


static error_t
tcpp03_write_ctrl(tcpp03_t *t, uint8_t ctrl)
{
  t->t_ctrl = ctrl;
  error_t err = i2c_write_u8(t->t_i2c, t->t_address, REG_CTRL, ctrl);
  if(err)
    t->t_err = err;
  return err;
}


// Low power mode while unattached and normal mode once attached, like
// ST's Type-C code does

error_t
tcpp03_source_on(tcpp03_t *t)
{
  error_t err = tcpp03_write_ctrl(t, CTRL_MODE_NORMAL | CTRL_GDC_OPEN);
  if(err)
    return err;
  usleep(2000); // Make sure the sink path is open before closing the source
  return tcpp03_write_ctrl(t, CTRL_MODE_NORMAL | CTRL_GDC_OPEN |
                           CTRL_GDP_CLOSED);
}


error_t
tcpp03_sink_on(tcpp03_t *t)
{
  return tcpp03_write_ctrl(t, CTRL_MODE_NORMAL);
}


error_t
tcpp03_detach(tcpp03_t *t)
{
  if(t->t_ctrl & CTRL_GDP_CLOSED) {
    // We were the source. There's no VBUS measurement here, ST's BSP
    // waits for VBUS to fall below 0.8V, up to 400ms
    error_t err = tcpp03_write_ctrl(t, CTRL_MODE_NORMAL | CTRL_GDC_OPEN |
                                    CTRL_VBUS_DISCHARGE);
    if(err)
      return err;
    usleep(100000);
    err = tcpp03_write_ctrl(t, CTRL_MODE_NORMAL | CTRL_GDC_OPEN);
    if(err)
      return err;
    usleep(30000);
  }
  return tcpp03_write_ctrl(t, CTRL_MODE_LOWPOWER);
}


static void
tcpp03_print_info(struct device *d, struct stream *st)
{
  tcpp03_t *t = (tcpp03_t *)d;
  static const char *modes[4] = {"hibernate", "normal", "low power", "?"};
  uint8_t flags = 0, ack = 0;

  error_t err = i2c_read_u8(t->t_i2c, t->t_address, REG_FLAGS, &flags);
  if(!err)
    err = i2c_read_u8(t->t_i2c, t->t_address, REG_ACK, &ack);
  if(err) {
    stprintf(st, "I2C error: %s\n", error_to_string(err));
    return;
  }

  // REG1 echoes REG0 (ST's driver decodes some of its fields differently,
  // but on this part the mode bits read back as written)
  stprintf(st, "Mode: %s  Source path: %s  Sink path: %s%s\n",
           modes[(ack & CTRL_MODE_MASK) >> 4],
           ack & CTRL_GDP_CLOSED ? "on" : "off",
           ack & CTRL_GDC_OPEN ? "off" : "on",
           ack & CTRL_VBUS_DISCHARGE ? "  Discharging" : "");
  if(ack != t->t_ctrl)
    stprintf(st, "Mismatch: wrote 0x%02x, reads back 0x%02x\n",
             t->t_ctrl, ack);
  stprintf(st, "Flags: 0x%02x%s%s%s%s%s%s\n", flags,
           flags & FLAG_VBUS_OK ? " VBUS-OK" : "",
           flags & FLAG_OCP_VBUS ? " VBUS-overcurrent" : "",
           flags & FLAG_OVP_VBUS ? " VBUS-overvoltage" : "",
           flags & FLAG_OVP_CC ? " CC-overvoltage" : "",
           flags & FLAG_OCP_VCONN ? " VCONN-overcurrent" : "",
           flags & FLAG_OTP ? " Overtemperature" : "");
  if(t->t_err)
    stprintf(st, "Last I2C error: %s\n", error_to_string(t->t_err));
}


static const device_class_t tcpp03_class = {
  .dc_class_name = "tcpp03",
  .dc_print_info = tcpp03_print_info,
};


tcpp03_t *
tcpp03_create(i2c_t *i2c, uint8_t address, gpio_t enable, const char *name)
{
  tcpp03_t *t = calloc(1, sizeof(tcpp03_t));
  t->t_i2c = i2c;
  t->t_address = address;

  if(enable != GPIO_UNUSED) {
    gpio_conf_output(enable, GPIO_PUSH_PULL, GPIO_SPEED_LOW, GPIO_PULL_NONE);
    gpio_set_output(enable, 1);
    usleep(1000);
  }

  // Low power: dead battery Rd off so the MCU can present Rp or Rd
  error_t err = tcpp03_write_ctrl(t, CTRL_MODE_LOWPOWER);
  if(err)
    printf("%s: Init failed: %s\n", name, error_to_string(err));

  t->t_dev.d_name = name;
  t->t_dev.d_class = &tcpp03_class;
  device_register(&t->t_dev);
  return t;
}
