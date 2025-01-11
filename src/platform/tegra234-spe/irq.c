#include "irq.h"

#include <stdlib.h>
#include <string.h>

#include <mios/mios.h>
#include <stdio.h>

#define NV_ADDRESS_MAP_LIC_CH2_BASE 0x03001000
#define NV_ADDRESS_MAP_LIC_CH3_BASE 0x03001800

uint32_t irq_current_mask = 0xffff;

const static uint32_t irq_trampoline_vic0[] = {
  0xe24ee004, //        sub     lr, lr, #4
  0xf96d0513, //        srsdb   sp!, #19
  0xf1020013, //        cps     #19
  0xe92d100f, //        push    {r0, r1, r2, r3, ip}
  0xe20d1004, //        and     r1, sp, #4
  0xe04dd001, //        sub     sp, sp, r1
  0xe92d4002, //        push    {r1, lr}
  0xe59f1020, //        ldr     r1, [pc, #32]
  0xe59f0020, //        ldr     r0, [pc, #32]
  0xe12fff31, //        blx     r1
  0xe3a01c0f, //        mov     r1, #3840       ; 0xf00
  0xe3401c02, //        movt    r1, #3074       ; 0xc02
  0xe5811000, //        str     r1, [r1]
  0xe8bd4002, //        pop     {r1, lr}
  0xe08dd001, //        add     sp, sp, r1
  0xe8bd100f, //        pop     {r0, r1, r2, r3, ip}
  0xf8bd0a00, //        rfeia   sp!
  //  0xdeadc0de, //        .word   0xdeadc0de  // Function
  //  0xc0dedbad, //        .word   0xc0dedbad  // Argument
};

const static uint32_t irq_trampoline_vic1[] = {
  0xe24ee004, //        sub     lr, lr, #4
  0xf96d0513, //        srsdb   sp!, #19
  0xf1020013, //        cps     #19
  0xe92d100f, //        push    {r0, r1, r2, r3, ip}
  0xe20d1004, //        and     r1, sp, #4
  0xe04dd001, //        sub     sp, sp, r1
  0xe92d4002, //        push    {r1, lr}
  0xe59f1028, //        ldr     r1, [pc, #40]
  0xe59f0028, //        ldr     r0, [pc, #40]
  0xe12fff31, //        blx     r1
  0xe3a01c0f, //        mov     r1, #3840       ; 0xf00
  0xe3401c02, //        movt    r1, #3074       ; 0xc02
  0xe5811000, //        str     r1, [r1]
  0xe3401c03, //        movt    r1, #3075       ; 0xc03
  0xe5811000, //        str     r1, [r1]
  0xe8bd4002, //        pop     {r1, lr}
  0xe08dd001, //        add     sp, sp, r1
  0xe8bd100f, //        pop     {r0, r1, r2, r3, ip}
  0xf8bd0a00, //        rfeia   sp!
  //  0xdeadc0de, //        .word   0xdeadc0de  // Function
  //  0xc0dedbad, //        .word   0xc0dedbad  // Argument
};


typedef struct lic_irq {
  void (*fn)(void *arg);
  void *arg;
} lic_irq_t;

#define LIC_IRQ_COUNT 544

static lic_irq_t lic_irq_vector[544];



static void
vic_enable(uint32_t vic_base, uint32_t irq, uint32_t priority, void *fn)
{
  reg_wr(vic_base + VIC_VECTOR_ADDRESS(irq), (intptr_t)fn);
  reg_wr(vic_base + VIC_PRIORITY(irq), priority);
  reg_wr(vic_base + VIC_INT_ENABLE, 1 << irq);
}

void
irq_enable_fn_arg(int irq, int level, void (*fn)(void *arg), void *arg)
{
  if(irq >= 0x10000) {
    // LIC

    uint32_t base;
    if(level == IRQ_LEVEL_NET) {
      base = NV_ADDRESS_MAP_LIC_CH3_BASE;
    } else if(level == IRQ_LEVEL_IO) {
      base = NV_ADDRESS_MAP_LIC_CH2_BASE;
    } else {
      panic("Invalid level %d for LIC irq", irq);
    }

    irq &= 0xffff;

    if(irq >= LIC_IRQ_COUNT)
      panic("lic bad irq %d", irq);

    lic_irq_vector[irq].fn = fn;
    lic_irq_vector[irq].arg = arg;

    int slice = irq >> 5;
    int index = irq & 0x1f;
    reg_wr(base + slice * 0x40 + 0xc, (1 << index));
    return;
  }

  int vic_idx = (irq >> 5) & 1;

  const void *template;
  size_t template_size;
  if(vic_idx == 0) {
    template = irq_trampoline_vic0;
    template_size = sizeof(irq_trampoline_vic0);
  } else {
    template = irq_trampoline_vic1;
    template_size = sizeof(irq_trampoline_vic1);
  }

  void *trampoline = malloc(template_size + sizeof(uint32_t) * 2);
  memcpy(trampoline, template, template_size);

  uint32_t *u32 = trampoline + template_size;
  u32[0] = (intptr_t)fn;
  u32[1] = (intptr_t)arg;

  // TODO: Flush I-cache
  vic_enable(VIC_BASE(vic_idx), irq & 0x1f, level, trampoline);
}

void
irq_enable_fn(int irq, int level, void (*fn)(void))
{
  irq_enable_fn_arg(irq, level, (void *)fn, NULL);
}

void
irq_disable(int irq)
{

}



static void
dispatch_lic_irq(uint32_t regbase)
{
  for(int s = 0; s < 16; s++) {
    uint32_t active = reg_rd(regbase + s * 0x40);

    while(active) {
      int which = 31 - __builtin_clz(active);
      lic_irq_t *li = &lic_irq_vector[s * 32 + which];
      li->fn(li->arg);
      active &= ~(1 << which);
    }
  }
}


static void
irq_lic_io(void)
{
  uint32_t base = NV_ADDRESS_MAP_LIC_CH2_BASE;
  dispatch_lic_irq(base);
}

static void
irq_lic_net(void)
{
  uint32_t base = NV_ADDRESS_MAP_LIC_CH3_BASE;
  dispatch_lic_irq(base);
}

static void  __attribute__((constructor(120)))
orin_spe_irq_init(void)
{
  extern void cpu_task_switch(void);
  vic_enable(VIC_BASE(0), 17, IRQ_LEVEL_SWITCH, cpu_task_switch);

  irq_enable_fn(28, IRQ_LEVEL_IO, irq_lic_io);
  irq_enable_fn(30, IRQ_LEVEL_NET, irq_lic_net);
}
