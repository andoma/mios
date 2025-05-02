#include <stdint.h>

extern uint32_t vectors[];

void __attribute__((section("bltext"),noinline,noreturn)) bl_start(void)
{
  void (*init)(void) __attribute__((noreturn))  = (void *)vectors[1];
  init();
}

void __attribute__((section("bltext"),noinline))
bl_nmi(void)
{
  while(1) {}
}

void __attribute__((section("bltext"),noinline))
bl_hard_fault(void)
{
  while(1) {}
}

void __attribute__((section("bltext"),noinline))
bl_mm_fault(void)
{
  while(1) {}
}

void __attribute__((section("bltext"),noinline))
bl_bus_fault(void)
{
  while(1) {}
}

void __attribute__((section("bltext"),noinline))
bl_usage_fault(void)
{
  while(1) {}
}

