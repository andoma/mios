/*
 * "vbus": the host's GPIO pins (vgpio.c), checked for the STM32
 * behaviours board code relies on: the output latch, reading back what
 * an output drives, open drain, edge IRQs.
 */

#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <mios/io.h>
#include <mios/task.h>

#include "hosttest.h"
#include "irq.h"
#include "vgpio.h"

// ---- GPIO

static int irq_count;

static void
count_irq(void *arg)
{
  irq_count++;
}


static int
test_gpio(void)
{
  int fails = 0;
  const gpio_t p = GPIO_PB(3);

  fails += !CHECK(vgpio_level(p) == 0, "floating input reads 0");
  gpio_conf_input(p, GPIO_PULL_UP);
  fails += !CHECK(gpio_get_input(p) == 1, "pull-up reads 1");

  // The latch is set while still an input, the pin drives it once it is
  // an output (illumination.c configures its chip selects this way)
  gpio_set_output(p, 0);
  fails += !CHECK(gpio_get_input(p) == 1, "latch drives nothing as input");
  fails += !CHECK(vgpio_driven(p) == -1, "input does not drive");
  gpio_conf_output(p, GPIO_PUSH_PULL, GPIO_SPEED_LOW, GPIO_PULL_NONE);
  fails += !CHECK(vgpio_driven(p) == 0, "output drives the latch");
  fails += !CHECK(gpio_get_input(p) == 0, "output reads back its level");
  gpio_set_output(p, 1);
  fails += !CHECK(vgpio_level(p) == 1, "output at 1");

  // Push-pull wins over something driving from outside
  vgpio_drive(p, 0);
  fails += !CHECK(vgpio_level(p) == 1, "push-pull 1 overrides outside 0");
  vgpio_release(p);

  // Open drain at 1 lets go: the outside or the pull decides
  const gpio_t od = GPIO_PB(4);
  gpio_conf_output(od, GPIO_OPEN_DRAIN, GPIO_SPEED_LOW, GPIO_PULL_UP);
  gpio_set_output(od, 1);
  fails += !CHECK(vgpio_driven(od) == -1, "open drain 1 does not drive");
  fails += !CHECK(vgpio_level(od) == 1, "open drain 1 pulled up");
  vgpio_drive(od, 0);
  fails += !CHECK(vgpio_level(od) == 0, "open drain 1 pulled low outside");
  vgpio_release(od);
  gpio_set_output(od, 0);
  fails += !CHECK(vgpio_level(od) == 0, "open drain 0 drives low");

  // Edge IRQs
  const gpio_t ip = GPIO_PC(5);
  irq_count = 0;
  gpio_conf_irq(ip, GPIO_PULL_UP, count_irq, NULL, GPIO_FALLING_EDGE,
                IRQ_LEVEL_IO);
  usleep(1000);
  fails += !CHECK(irq_count == 0, "configuring is not an edge");
  vgpio_drive(ip, 0);
  usleep(1000);
  fails += !CHECK(irq_count == 1, "falling edge: %d IRQs, want 1",
                  irq_count);
  vgpio_drive(ip, 1);
  usleep(1000);
  fails += !CHECK(irq_count == 1, "rising edge ignored: %d IRQs",
                  irq_count);
  gpio_conf_irq_edge(ip, GPIO_BOTH_EDGES);
  vgpio_drive(ip, 0);
  vgpio_release(ip);  // Back to the pull-up: a rising edge
  usleep(1000);
  fails += !CHECK(irq_count == 3, "both edges: %d IRQs, want 3", irq_count);
  gpio_disconnect(ip);
  vgpio_drive(ip, 0);
  usleep(1000);
  fails += !CHECK(irq_count == 3, "disconnected pin fires no IRQ");
  vgpio_release(ip);

  fails += !CHECK(vgpio_level(-1) == 0 && vgpio_level(HOST_GPIO_COUNT) == 0,
                  "pins out of range read 0");
  return fails;
}


static int
suite_vbus(void)
{
  int fails = 0;
  fails += test_gpio();
  return fails;
}

HOSTTEST_SUITE("vbus", suite_vbus, 0);
