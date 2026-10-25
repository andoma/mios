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
#include <mios/task.h>

#include "irq.h"

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
  int (*t_vbus_mv)(void *opaque);
  void *t_vbus_opaque;

  task_waitable_t t_flag_wait;
  uint8_t t_flag_event;     // Protected by IRQ_LEVEL_IO
  uint8_t t_last_faults;
  uint32_t t_faults[5];     // Per fault flag bit
};

static const char *fault_names[5] = {
  "VCONN-overcurrent", "VBUS-overcurrent", "VBUS-overvoltage",
  "CC-overvoltage", "Overtemperature"
};


void
tcpp03_set_vbus_monitor(tcpp03_t *t, int (*vbus_mv)(void *opaque),
                        void *opaque)
{
  t->t_vbus_mv = vbus_mv;
  t->t_vbus_opaque = opaque;
}


// Wait for VBUS to fall below vSafe0V (0.8V), like ST's BSP
static void
tcpp03_wait_discharged(tcpp03_t *t)
{
  if(t->t_vbus_mv == NULL) {
    usleep(100000);
    return;
  }
  for(int i = 0; i < 50; i++) {
    if(t->t_vbus_mv(t->t_vbus_opaque) < 800)
      return;
    usleep(10000);
  }
  printf("%s: VBUS did not discharge\n", t->t_dev.d_name);
}


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
    // We were the source
    error_t err = tcpp03_write_ctrl(t, CTRL_MODE_NORMAL | CTRL_GDC_OPEN |
                                    CTRL_VBUS_DISCHARGE);
    if(err)
      return err;
    tcpp03_wait_discharged(t);
    err = tcpp03_write_ctrl(t, CTRL_MODE_NORMAL | CTRL_GDC_OPEN);
    if(err)
      return err;
    usleep(30000);
  }
  return tcpp03_write_ctrl(t, CTRL_MODE_LOWPOWER);
}


static void
tcpp03_flag_irq(void *arg)
{
  tcpp03_t *t = arg;
  t->t_flag_event = 1;
  task_wakeup(&t->t_flag_wait, 0);
}


// FLGn goes low on a fault and stays low while it persists (an
// overvoltage condition recovers by itself once VBUS drops)
__attribute__((noreturn))
static void *
tcpp03_flag_thread(void *arg)
{
  tcpp03_t *t = arg;

  while(1) {
    int q = irq_forbid(IRQ_LEVEL_IO);
    while(!t->t_flag_event)
      task_sleep(&t->t_flag_wait);
    t->t_flag_event = 0;
    irq_permit(q);

    // In hibernate and low power modes FLGn only signals VBUS presence
    if((t->t_ctrl & CTRL_MODE_MASK) != CTRL_MODE_NORMAL)
      continue;

    uint8_t flags;
    if(i2c_read_u8(t->t_i2c, t->t_address, REG_FLAGS, &flags))
      continue;
    const uint8_t faults = flags & 0x1f;
    if(faults == 0 || faults == t->t_last_faults)
      continue;
    t->t_last_faults = faults;

    printf("%s: Fault:", t->t_dev.d_name);
    for(int i = 0; i < 5; i++) {
      if(faults & (1 << i)) {
        t->t_faults[i]++;
        printf(" %s", fault_names[i]);
      }
    }
    printf("\n");
  }
}


void
tcpp03_set_flag_gpio(tcpp03_t *t, gpio_t flg)
{
  thread_create(tcpp03_flag_thread, t, 1024, t->t_dev.d_name, 0, 4);
  gpio_conf_irq(flg, GPIO_PULL_UP, tcpp03_flag_irq, t,
                GPIO_FALLING_EDGE, IRQ_LEVEL_IO);
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
  for(int i = 0; i < 5; i++) {
    if(t->t_faults[i])
      stprintf(st, "%s faults: %d\n", fault_names[i], t->t_faults[i]);
  }
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
