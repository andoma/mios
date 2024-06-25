#pragma once

#define EXTI_BASE         0x40010400

#define EXTI_IMR1       (EXTI_BASE + 0x00)
#define EXTI_RTSR1      (EXTI_BASE + 0x08)
#define EXTI_FTSR1      (EXTI_BASE + 0x0c)
#define EXTI_PR1        (EXTI_BASE + 0x14)

#define SYSCFG_BASE    0x40010000

#define SYSCFG_EXTICR(x) (SYSCFG_BASE + 8 + 4 * (x))
