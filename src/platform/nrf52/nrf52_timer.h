#pragma once

#define TIMER0_BASE 0x40008000
#define TIMER1_BASE 0x40009000
#define TIMER2_BASE 0x4000a000
#define TIMER3_BASE 0x4001a000
#define TIMER4_BASE 0x4001b000

#define TIMER0_IRQ  8
#define TIMER1_IRQ  9
#define TIMER2_IRQ  10
#define TIMER3_IRQ  26
#define TIMER4_IRQ  27

#define TIMER_TASKS_START        0x000
#define TIMER_TASKS_STOP         0x004
#define TIMER_TASKS_COUNT        0x008
#define TIMER_TASKS_CLEAR        0x00c
#define TIMER_TASKS_SHUTDOWN     0x010
#define TIMER_TASKS_CAPTURE(x)  (0x040 + (x) * 4)

#define TIMER_EVENTS_COMPARE(x) (0x140 + (x) * 4)

#define TIMER_SHORTS             0x200
#define TIMER_INTENSET           0x304
#define TIMER_INTENCLR           0x308
#define TIMER_MODE               0x504
#define TIMER_BITMODE            0x508
#define TIMER_PRESCALER          0x510

#define TIMER_CC(x)             (0x540 + (x) * 4)

