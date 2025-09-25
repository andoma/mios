#pragma once

static inline void dmb(void)
{
  asm volatile("dmb sy" ::: "memory");
}

static inline void dsb(void)
{
  asm volatile("dsb sy" ::: "memory");
}
