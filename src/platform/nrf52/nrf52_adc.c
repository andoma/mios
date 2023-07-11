#include "nrf52_adc.h"
#include "nrf52_reg.h"

#include <mios/task.h>

#include "irq.h"

static mutex_t adc_mutex = MUTEX_INITIALIZER("adc");
static task_waitable_t adc_waitq = WAITABLE_INITIALIZER("adc");
static int adc_status;

void
irq_7(void)
{
  if(reg_rd(ADC_EVENTS_STARTED)) {
    reg_wr(ADC_EVENTS_STARTED, 0);
    reg_wr(ADC_TASKS_SAMPLE, 1);
  } else if(reg_rd(ADC_EVENTS_END)) {
    reg_wr(ADC_EVENTS_END, 0);
    reg_wr(ADC_TASKS_STOP, 1);
    adc_status = 2;
    task_wakeup(&adc_waitq, 0);
  }
}


void
adc_sample(int16_t *output, size_t count)
{
  mutex_lock(&adc_mutex);
  if(adc_status == 0) {
    irq_enable(7, IRQ_LEVEL_IO);
    adc_status = 1;
    reg_wr(ADC_RESOLUTION, 3);
    reg_wr(ADC_INTENSET, 3);
  }
  reg_wr(ADC_RESULT_PTR, (intptr_t)output);
  reg_wr(ADC_RESULT_MAXCNT, count);

  reg_wr(ADC_ENABLE, 1);

  int q = irq_forbid(IRQ_LEVEL_IO);

  reg_wr(ADC_TASKS_START, 1);
  while(adc_status != 2) {
    task_sleep(&adc_waitq);
  }
  while(reg_rd(ADC_EVENTS_STOPPED) == 0) {}
  reg_wr(ADC_EVENTS_STOPPED, 0);

  irq_permit(q);
  adc_status = 1;
  reg_wr(ADC_ENABLE, 0);
  mutex_unlock(&adc_mutex);
}


void
adc_lock(void)
{
  mutex_lock(&adc_mutex);
}

void
adc_unlock(void)
{
  mutex_unlock(&adc_mutex);
}
