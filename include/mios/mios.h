#pragma once

void panic(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));

void panic_frame(void *frame, const char *fmt, ...) __attribute__((noreturn, format(printf, 2, 3)));

void fini(void);

void reboot(void);

void shutdown_notification(const char *reason);

void dfu(void) __attribute__((noreturn));

#define likely(x)       __builtin_expect((x),1)
#define unlikely(x)     __builtin_expect((x),0)

// Place a function in the "fastcode" linker section. On platforms with a
// dedicated fast-code memory (e.g. STM32N6 ITCM) the linker script routes
// this section there; on others it falls into the regular .text/FLASH.
#define FAST __attribute__((section("fastcode")))

#define MIOS_GLUE(a, b) a ## b
#define MIOS_JOIN(a, b) MIOS_GLUE(a, b)

typedef struct handler {
  void (*fn)(void *arg);
  void *arg;
} handler_t;
