#pragma once

#define RNG_BASE         0x4000d000
#define RNG_TASKS_START    (RNG_BASE + 0x000)
#define RNG_TASKS_STOP     (RNG_BASE + 0x004)
#define RNG_EVENTS_VALRDY  (RNG_BASE + 0x100)
#define RNG_CONFIG         (RNG_BASE + 0x504)
#define RNG_VALUE          (RNG_BASE + 0x508)
