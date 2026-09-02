/*
 * hostlib platform glue: map a heap. No console/stdin, no signals -- the
 * object is driven by a harness through the ABI in libmios.c. Everything
 * else (boot, IRQs, clock, scheduler) is the shared host CPU layer.
 */
#include <mios/mios.h>
#include "cpu.h"

#define HOSTLIB_HEAP_SIZE (64 * 1024 * 1024)

static void __attribute__((constructor(120)))
hostlib_heap_init(void)
{
  host_map_heap(HOSTLIB_HEAP_SIZE);
}
