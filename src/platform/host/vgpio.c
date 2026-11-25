#include <stdint.h>
#include <stddef.h>

#include <mios/io.h>
#include <mios/mios.h>

#include "irq.h"
#include "vgpio.h"

// See vgpio.h

#define MODE_INPUT    0
#define MODE_OUTPUT   1
#define MODE_AF       2
#define MODE_ANALOG   3

typedef struct vpin {
  void (*cb)(void *arg);
  void *arg;
  uint8_t mode;
  uint8_t latch;
  uint8_t open_drain;
  uint8_t pull;
  uint8_t ext_on;        // Driven from outside, see vgpio_drive()
  uint8_t ext;           // ... with this level
  uint8_t level;         // Last computed level, for edge detection
  uint8_t edge;          // GPIO_*_EDGE, 0 when no IRQ is configured
  uint8_t irq_level;
  uint8_t pending;
} vpin_t;

static vpin_t pins[HOST_GPIO_COUNT];

// One host IRQ line per IRQ level used by a pin, allocated on first use.
// Holds the line + 1, so 0 is "not allocated" and the zeroed array (pins
// can be configured from constructors) needs no init.
#define NUM_LEVELS 8
static uint8_t level_irq[NUM_LEVELS];


static vpin_t *
pin_get(gpio_t gpio)
{
  if(gpio < 0 || gpio >= HOST_GPIO_COUNT)
    return NULL;
  return &pins[gpio];
}


static int
pin_driven(const vpin_t *p)
{
  if(p->mode != MODE_OUTPUT)
    return -1;
  if(p->open_drain && p->latch)
    return -1;
  return p->latch;
}


static int
pin_level(const vpin_t *p)
{
  int d = pin_driven(p);
  if(d >= 0)
    return d;
  if(p->ext_on)
    return p->ext;
  if(p->mode == MODE_ANALOG)
    return 0;
  return p->pull == GPIO_PULL_UP;
}


static void
level_irq_handler(void *arg)
{
  const unsigned int level = (intptr_t)arg;

  for(int i = 0; i < HOST_GPIO_COUNT; i++) {
    vpin_t *p = &pins[i];
    if(!p->pending || p->irq_level != level)
      continue;
    p->pending = 0;
    if(p->cb != NULL)
      p->cb(p->arg);
  }
}


// Called with IRQs forbidden. A pin's IRQ level was allocated by
// gpio_conf_irq() (which runs with IRQs permitted), so this only pends.
static void
pin_update(vpin_t *p)
{
  const uint8_t level = pin_level(p);
  if(level == p->level)
    return;
  p->level = level;

  if(!p->edge || p->cb == NULL)
    return;

  const int fire = level ? (p->edge & GPIO_RISING_EDGE)
                         : (p->edge & GPIO_FALLING_EDGE);
  if(!fire)
    return;
  p->pending = 1;
  host_irq_pend(level_irq[p->irq_level] - 1);
}


static void
alloc_level_irq(unsigned int level)
{
  if(level_irq[level] == 0)
    level_irq[level] = 1 + host_irq_alloc(level, level_irq_handler,
                                          (void *)(intptr_t)level);
}


void
gpio_conf_input(gpio_t gpio, gpio_pull_t pull)
{
  vpin_t *p = pin_get(gpio);
  if(p == NULL)
    return;
  const int q = irq_forbid(IRQ_LEVEL_ALL);
  p->mode = MODE_INPUT;
  p->pull = pull;
  pin_update(p);
  irq_permit(q);
}


void
gpio_conf_output(gpio_t gpio, gpio_output_type_t type,
                 gpio_output_speed_t speed, gpio_pull_t pull)
{
  vpin_t *p = pin_get(gpio);
  if(p == NULL)
    return;
  const int q = irq_forbid(IRQ_LEVEL_ALL);
  p->mode = MODE_OUTPUT;
  p->open_drain = type == GPIO_OPEN_DRAIN;
  p->pull = pull;
  pin_update(p);
  irq_permit(q);
}


void
gpio_dir_output(gpio_t gpio)
{
  vpin_t *p = pin_get(gpio);
  if(p == NULL)
    return;
  const int q = irq_forbid(IRQ_LEVEL_ALL);
  p->mode = MODE_OUTPUT;
  pin_update(p);
  irq_permit(q);
}


void
gpio_set_output(gpio_t gpio, int on)
{
  vpin_t *p = pin_get(gpio);
  if(p == NULL)
    return;
  const int q = irq_forbid(IRQ_LEVEL_ALL);
  p->latch = !!on;
  pin_update(p);
  irq_permit(q);
}


int
gpio_get_input(gpio_t gpio)
{
  vpin_t *p = pin_get(gpio);
  if(p == NULL)
    return 0;
  const int q = irq_forbid(IRQ_LEVEL_ALL);
  int r = pin_level(p);
  irq_permit(q);
  return r;
}


void
gpio_conf_af(gpio_t gpio, int af, gpio_output_type_t type,
             gpio_output_speed_t speed, gpio_pull_t pull)
{
  vpin_t *p = pin_get(gpio);
  if(p == NULL)
    return;
  const int q = irq_forbid(IRQ_LEVEL_ALL);
  p->mode = MODE_AF;
  p->open_drain = type == GPIO_OPEN_DRAIN;
  p->pull = pull;
  pin_update(p);
  irq_permit(q);
}


void
gpio_conf_analog(gpio_t gpio, gpio_pull_t pull)
{
  vpin_t *p = pin_get(gpio);
  if(p == NULL)
    return;
  const int q = irq_forbid(IRQ_LEVEL_ALL);
  p->mode = MODE_ANALOG;
  p->pull = pull;
  pin_update(p);
  irq_permit(q);
}


void
gpio_conf_irq(gpio_t gpio, gpio_pull_t pull,
              void (*cb)(void *arg), void *arg,
              gpio_edge_t edge, int level)
{
  vpin_t *p = pin_get(gpio);
  if(p == NULL)
    return;
  if(level < 1 || level >= NUM_LEVELS)
    panic("gpio_conf_irq: Bad IRQ level %d", level);

  alloc_level_irq(level);

  const int q = irq_forbid(IRQ_LEVEL_ALL);
  p->mode = MODE_INPUT;
  p->pull = pull;
  p->cb = cb;
  p->arg = arg;
  p->irq_level = level;
  p->pending = 0;
  p->level = pin_level(p);  // Configuring is not an edge
  p->edge = edge;
  irq_permit(q);
}


void
gpio_conf_irq_edge(gpio_t gpio, gpio_edge_t edge)
{
  vpin_t *p = pin_get(gpio);
  if(p == NULL)
    return;
  const int q = irq_forbid(IRQ_LEVEL_ALL);
  p->edge = edge;
  irq_permit(q);
}


void
gpio_disconnect(gpio_t gpio)
{
  vpin_t *p = pin_get(gpio);
  if(p == NULL)
    return;
  const int q = irq_forbid(IRQ_LEVEL_ALL);
  p->mode = MODE_ANALOG;
  p->pull = GPIO_PULL_NONE;
  p->edge = 0;
  p->cb = NULL;
  p->pending = 0;
  pin_update(p);
  irq_permit(q);
}


int
vgpio_driven(gpio_t gpio)
{
  vpin_t *p = pin_get(gpio);
  if(p == NULL)
    return -1;
  const int q = irq_forbid(IRQ_LEVEL_ALL);
  int r = pin_driven(p);
  irq_permit(q);
  return r;
}


int
vgpio_level(gpio_t gpio)
{
  return gpio_get_input(gpio);
}


int
vgpio_floating(gpio_t gpio)
{
  vpin_t *p = pin_get(gpio);
  if(p == NULL)
    return 1;
  const int q = irq_forbid(IRQ_LEVEL_ALL);
  int r = pin_driven(p) < 0 && !p->ext_on &&
    (p->mode == MODE_ANALOG || p->pull == GPIO_PULL_NONE);
  irq_permit(q);
  return r;
}


void
vgpio_drive(gpio_t gpio, int level)
{
  vpin_t *p = pin_get(gpio);
  if(p == NULL)
    return;
  const int q = irq_forbid(IRQ_LEVEL_ALL);
  p->ext_on = 1;
  p->ext = !!level;
  pin_update(p);
  irq_permit(q);
}


void
vgpio_release(gpio_t gpio)
{
  vpin_t *p = pin_get(gpio);
  if(p == NULL)
    return;
  const int q = irq_forbid(IRQ_LEVEL_ALL);
  p->ext_on = 0;
  pin_update(p);
  irq_permit(q);
}

