#pragma once

#include <stddef.h>
#include <stdint.h>

#define ADC_BASE 0x40007000

#define ADC_TASKS_START  (ADC_BASE + 0x000)
#define ADC_TASKS_SAMPLE (ADC_BASE + 0x004)
#define ADC_TASKS_STOP   (ADC_BASE + 0x008)
#define ADC_TASKS_CALIB  (ADC_BASE + 0x00c)

#define ADC_EVENTS_STARTED  (ADC_BASE + 0x100)
#define ADC_EVENTS_END      (ADC_BASE + 0x104)
#define ADC_EVENTS_DONE     (ADC_BASE + 0x108)
#define ADC_EVENTS_RESULT   (ADC_BASE + 0x10c)
#define ADC_EVENTS_CALIB    (ADC_BASE + 0x110)
#define ADC_EVENTS_STOPPED  (ADC_BASE + 0x114)

#define ADC_EVENTS_LIMITH(x) (ADC_BASE + 0x118 + (x) * 8)
#define ADC_EVENTS_LIMITL(x) (ADC_BASE + 0x11c + (x) * 8)

#define ADC_INTEN       (ADC_BASE + 0x300)
#define ADC_INTENSET    (ADC_BASE + 0x304)
#define ADC_INTENCLR    (ADC_BASE + 0x308)
#define ADC_STATUS      (ADC_BASE + 0x400)
#define ADC_ENABLE      (ADC_BASE + 0x500)

#define ADC_PSELP(x)    (ADC_BASE + 0x510 + (x) * 16)
#define ADC_PSELN(x)    (ADC_BASE + 0x514 + (x) * 16)
#define ADC_CONFIG(x)   (ADC_BASE + 0x518 + (x) * 16)
#define ADC_LIMIT(x)    (ADC_BASE + 0x51c + (x) * 16)

#define ADC_RESOLUTION  (ADC_BASE + 0x5f0)
#define ADC_OVERSAMPLE  (ADC_BASE + 0x5f4)
#define ADC_SAMPLERATE  (ADC_BASE + 0x5f8)

#define ADC_RESULT_PTR     (ADC_BASE + 0x62c)
#define ADC_RESULT_MAXCNT  (ADC_BASE + 0x630)
#define ADC_RESULT_AMOUNT  (ADC_BASE + 0x634)

#define ADC_PIN_NC     0
#define ADC_PIN_AIN(x) ((x) + 1)
#define ADC_PIN_VDD    9


void adc_sample(int16_t *output, size_t count);

void adc_lock(void);

void adc_unlock(void);
