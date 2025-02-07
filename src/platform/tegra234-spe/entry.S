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
        b prefetch_abort0
        b data_abort0
        nop
        b irq
        b fiq

        .global start
start:
        ldr r0, =#0x0c480000  // Run addr
        ldr r1, =#0x70000000  // Load addr
        ldr r2, =#0x0c4c0000  // End (+256kb)
1:      ldr r3, [r1]
        str r3, [r0]
        adds r0, #4
        adds r1, #4
        cmp r2, r0
        bne 1b

        ldr     r0, =#relocation_label
        bx r0

relocation_label:
        // Disable cache & MMU
        mrc p15, 0, r0, c1, c0, 0
        bic r0, r0, #0x05
        bic r0, r0, #0x1000
        dsb
        mcr p15, 0, r0, c1, c0, 0
        isb

        // setup vectors in EVP
        ldr r1, =#0x0c000000
        ldr r0, =#undef_instruction
        str r0, [r1, 0x24]
        ldr r0, =#swi
        str r0, [r1, 0x28]
        ldr r0, =#prefetch_abort
        str r0, [r1, 0x2c]
        ldr r0, =#data_abort0
        str r0, [r1, 0x30]
        ldr r0, =#irq
        str r0, [r1, 0x38]
        ldr r0, =#fiq
        str r0, [r1, 0x3c]

        cps #MODE_IRQ // Enter IRQ mode
        ldr sp, =_irqstack_end

        cps #MODE_ABT
        ldr sp, =_abtstack_end

        cps #MODE_SVC
        ldr sp, =_svcstack_end

        // Configure VIC to provide handler addr for IRQ
        mrc     p15, 0, r0, c1, c0, 0
        orr     r0, r0, #(0x1 << 24)
        mcr     p15, 0, r0, c1, c0, 0

        cpsie a

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
prefetch_abort0:
        push {r0-r12}
        sub r0,lr,#8
        mrs r1, spsr
        mov r2, sp
        b prefetch_abort
data_abort0:
        push {r0-r12}
        sub r0,lr,#8
        mrs r1, spsr
        mov r2, sp
        b data_abort


fiq:
        b .
irq:
        b .

        .global cpu_task_switch
cpu_task_switch:
        sub lr, lr, #4
        srsdb #MODE_SYS!
        cps #MODE_SYS
        push {r0-r3, r12, lr}
        push {r4-r11}
        mov r0, sp
        cps #MODE_SVC

        mov r4, lr
        bl task_switch
        mov lr, r4

        clrex

        mov r2, #(1 << 17)
        ldr r1, =0x0c020000
        str r2, [r1, 0x1c]
        str r2, [r1, 0xf00]

        cps #MODE_SYS
        mov sp, r0
        pop {r4-r11}
	pop {r0-r3, r12, lr}
	rfeia sp!
        .pool

irq_vic0:
        sub lr, lr, #4
        srsdb #MODE_SVC!
        cps #MODE_SVC
        push {r0-r3, r12}
        and r1, sp, #4
        sub sp, sp, r1
        push {r1, lr}
        ldr r1, =#0xdeadc0de
        ldr r0, =#0xc0dedbad
        blx r1
        mov r1, #0xf00
        movt r1, #0xc02
        str r1, [r1]
        pop {r1, lr}
        add sp, sp, r1
        pop {r0-r3,r12}
        rfefd sp!
        .pool

irq_vic1:
        sub lr, lr, #4
        srsdb #MODE_SVC!
        cps #MODE_SVC
        push {r0-r3, r12}
        and r1, sp, #4
        sub sp, sp, r1
        push {r1, lr}
        ldr r1, =#0xdeadc0de
        ldr r0, =#0xc0dedbad
        blx r1
        mov r1, #0xf00
        movt r1, #0xc02
        str r1, [r1]
        movt r1, #0xc03
        str r1, [r1]
        pop {r1, lr}
        add sp, sp, r1
        pop {r0-r3,r12}
        rfefd sp!
        .pool
