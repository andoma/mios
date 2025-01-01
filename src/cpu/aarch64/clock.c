#include <unistd.h>
#include <mios/timer.h>

uint64_t clock_get_irq_blocked(void)
{
  return 0;
}

uint64_t clock_get(void)
{
  return 0;
}

void
timer_arm_abs(timer_t *t, uint64_t expire)
{

}
