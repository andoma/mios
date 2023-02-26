#pragma once

#define RTC0_BASE 0x4000b000
#define RTC1_BASE 0x40011000
#define RTC2_BASE 0x40024000

#define RTC_TASKS_START        0x000
#define RTC_TASKS_STOP         0x004
#define RTC_TASKS_CLEAR        0x008
#define RTC_TASKS_TRIG_OVRFLW  0x00c
#define RTC_EVENTS_TICK        0x100
#define RTC_EVENTS_OVRFLW      0x104
#define RTC_EVENTS_COMPARE(x) (0x140 + (x) * 4)

#define RTC_INTENSET           0x304
#define RTC_INTENCLR           0x308
#define RTC_EVTEN              0x340
#define RTC_EVTENSET           0x344
#define RTC_EVTENCLR           0x348

#define RTC_COUNTER            0x504
#define RTC_PRESCALER          0x508

#define RTC_CC(x)             (0x540 + (x) * 4)


