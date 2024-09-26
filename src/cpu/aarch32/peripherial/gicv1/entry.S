#include "asm.h"

        .syntax unified

        .section    .vector_table,"a",%progbits
        .align      2
        .global     vector_table
        .type   vectors, %object
vector_table:
        b start
        b undef_instruction
        b swi
        b prefetch_abort
        b data_abort0
        nop
        b irq
        b fiq

        .global start
start:
        cpsid if
	ldr r0, =vector_table
	mcr p15,0,r0,c12,c0,0

        cps #MODE_IRQ // Enter IRQ mode
        ldr sp, =_irqstack_end

        cps #MODE_ABT
        ldr sp, =_abtstack_end

        cps #MODE_SVC
        ldr sp, =_svcstack_end

        bl init
        cps #MODE_SYS
        cpsie i
        isb
1:      wfi
        b 1b


undef_instruction:
        b .


swi:
        b .


prefetch_abort:
        b .


data_abort0:
        push {r0-r12}
        sub r0,lr,#8
        mrs r1, spsr
        mov r2, sp
        b data_abort


irq:
        sub lr, lr, #4
        srsdb #MODE_SYS!
        cps #MODE_SYS
        push {r0-r3, r12, lr}
       	cps #MODE_IRQ

	and r3, sp, #4
	sub sp, sp, r3
	push {r2, r3}

        mrc p15,#4,r0,c15,c0,0
        ldr r0, [r0, #0x10c]
        push {r0, lr}

        ldr r1, =#irqvector
        add r1, r1, r0, lsl #3
        ldm r1!,{r0,r3}
        blx r3

        pop {r0, lr}
        mrc p15,4,r1,c15,c0,0
        str r0, [r1, #0x110]

	pop {r2, r3}
	add sp, sp, r3
       	cps #MODE_SYS
	pop {r0-r3, r12, lr}
	rfeia sp!

fiq:
        b .

        .global cpu_task_switch
cpu_task_switch:
        cps #MODE_SYS
        push {r4-r11}
        mov r0, sp
       	cps #MODE_IRQ

        mov r4, lr
        bl task_switch
        mov lr, r4

        cps #MODE_SYS
        mov sp, r0
        pop {r4-r11}
       	cps #MODE_IRQ
        bx lr
