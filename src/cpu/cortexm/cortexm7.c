#include "cpu.h"
#include "cache.h"

#include <mios/mios.h>
#include <stdio.h>

static volatile uint32_t * const SHCSR    = (uint32_t *)0xe000ed24;
static volatile uint32_t * const FPCCR    = (uint32_t *)0xe000ef34;




static volatile uint32_t * const SCB_CCR     = (uint32_t *)0xe000ed14;
static volatile uint32_t * const SCB_CCSIDR  = (uint32_t *)0xe000ed80;
static volatile uint32_t * const SCB_CCSELR  = (uint32_t *)0xe000ed84;

static volatile uint32_t * const SCB_ICIALLU  = (uint32_t *)0xe000ef50;
static volatile uint32_t * const SCB_DCIMVAC  = (uint32_t *)0xe000ef5c;
static volatile uint32_t * const SCB_DCISW    = (uint32_t *)0xe000ef60;
static volatile uint32_t * const SCB_DCCMVAC  = (uint32_t *)0xe000ef68;
static volatile uint32_t * const SCB_DCCSW    = (uint32_t *)0xe000ef6c;
static volatile uint32_t * const SCB_DCCIMVAC = (uint32_t *)0xe000ef70;



void
icache_invalidate(void)
{
  asm volatile("dsb\n\tisb");
  *SCB_ICIALLU = 0;
  asm volatile("dsb\n\tisb");
}


static void
icache_disable(void)
{
  asm volatile("dsb\n\tisb");
  *SCB_CCR &= ~(1 << 17);
  asm volatile("dsb\n\tisb");
}

static void
icache_enable(void)
{
  *SCB_CCSELR = 1;  // Select L1 Dcache
  asm volatile("dsb");

  uint32_t ccsidr = *SCB_CCSIDR;

  uint32_t sets = ((ccsidr >> 13) & 0x7fff) + 1;
  uint32_t ways = ((ccsidr >> 3) & 0x3ff) + 1;

  printf("I-Cache %dkB (Sets:%d Ways:%d)\n",
         (CACHE_LINE_SIZE * sets * ways) / 1024, sets, ways);

  icache_invalidate();
  *SCB_CCR |= (1 << 17);
  asm volatile("dsb\n\tisb");
}


void
dcache_op(void *addr, size_t size, uint32_t flags)
{
  uint32_t begin = (intptr_t)addr;
  uint32_t end = (intptr_t)addr + size;

  begin &= ~(CACHE_LINE_SIZE - 1);
  end += (CACHE_LINE_SIZE - 1);
  end &= ~(CACHE_LINE_SIZE - 1);

  asm volatile("dsb");

  volatile uint32_t *reg;
  if(flags == DCACHE_CLEAN) {
    reg = SCB_DCCMVAC;
  } else if(flags == DCACHE_INVALIDATE) {
    reg = SCB_DCIMVAC;
  } else if(flags == (DCACHE_INVALIDATE | DCACHE_CLEAN)) {
    reg = SCB_DCCIMVAC;
  } else {
    return;
  }

  while(begin < end) {
    *reg = begin;
    begin += CACHE_LINE_SIZE;
  }
  asm volatile("dsb\n\tisb");
}


static void
dcache_enable(void)
{
  *SCB_CCSELR = 0;  // Select L1 Dcache

  asm volatile("dsb");
  uint32_t ccsidr = *SCB_CCSIDR;

  uint32_t sets = ((ccsidr >> 13) & 0x7fff) + 1;
  uint32_t ways = ((ccsidr >> 3) & 0x3ff) + 1;

  printf("D-Cache %dkB (Sets:%d Ways:%d)\n",
         (CACHE_LINE_SIZE * sets * ways) / 1024, sets, ways);

  // Invalidate entire D-cache before we enable
  for(uint32_t s = 0; s < sets; s++) {
    for(uint32_t w = 0; w < ways; w++) {
      *SCB_DCISW = (w << 30) | (s << 5);
    }
  }
  asm volatile("dsb");
  *SCB_CCR |= (1 << 16);
  asm volatile("dsb\n\tisb");
}


static void
dcache_disable(void)
{
  *SCB_CCSELR = 0;  // Select L1 Dcache

  asm volatile("dsb");
  uint32_t ccsidr = *SCB_CCSIDR;

  uint32_t sets = ((ccsidr >> 13) & 0x7fff) + 1;
  uint32_t ways = ((ccsidr >> 3) & 0x3ff) + 1;

  // Clean entire D-cache before we disable
  for(uint32_t s = 0; s < sets; s++) {
    for(uint32_t w = 0; w < ways; w++) {
      *SCB_DCCSW = (w << 30) | (s << 5);
    }
  }
  asm volatile("dsb\n\tisb");
  *SCB_CCR &= ~(1 << 16);
  asm volatile("dsb\n\tisb");
}


static void __attribute__((constructor(150)))
cortexm7_init(void)
{
  *FPCCR = 0; // No FPU lazy switching, we deal with it ourselves
  *SHCSR |= 0x7 << 16; // Enable UsageFault, BusFault, MemFault handlers

  icache_enable();
  dcache_enable();
}


static void __attribute__((destructor(150)))
cortexm7_fini(void)
{
  dcache_disable();
  icache_disable();

  *SHCSR &= ~(0x7 << 16); // Disable UsageFault, BusFault, MemFault handlers
}
