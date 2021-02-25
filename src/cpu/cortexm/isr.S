        .syntax unified
        .text
        .thumb
        .thumb_func
        .global start
start:
        cpsid i     // Disable interrupts
        bl init
        movs r0, #2  // Threaded mode
        msr control, r0
        isb
        ldr r0, =idle_stack + 128
        msr psp, r0
        isb
        cpsie i     // Enable interrupts, off we go
        isb
idle:   wfi
        b idle

        .thumb_func
pendsv:
        push {lr}
        mrs r0, psp
        isb
#ifdef __thumb2__
        stmdb r0!, {r4-r11}
#else
        subs	r0, #16
        stmia	r0!,{r4-r7}
        mov	r4, r8
        mov	r5, r9
        mov	r6, r10
        mov	r7, r11
        subs	r0, #32
        stmia	r0!,{r4-r7}
        subs	r0, #16
#endif
        bl task_switch
#ifdef __thumb2__
        ldmfd r0!, {r4-r11}
#else
        ldmia	r0!,{r4-r7}
        mov	r8, r4
        mov	r9, r5
        mov	r10, r6
        mov	r11, r7
        ldmia	r0!,{r4-r7}
#endif
        msr psp, r0
        isb
        pop {pc}

        .section    .isr_vector,"aw",%progbits
        .align      2
        .global     vectors
        .type   vectors, %object


        .altmacro
        .macro insert_irq number
        .long irq_\number
        .endm

vectors:
        .long _sdata          // Main stack (MSP) start before data-section
        .long start
        .long exc_nmi
        .long exc_hard_fault

        .long exc_mm_fault
        .long exc_bus_fault
        .long exc_usage_fault
        .long exc_reserved

        .long exc_reserved
        .long exc_reserved
        .long exc_reserved
        .long exc_svc

        .long exc_reserved
        .long exc_reserved
        .long pendsv
        .long exc_systick

        .set i,0
        .rept 240
        insert_irq %i
        .set i, i+1
        .endr

        .size vectors, . - vectors

        .bss
        .align 5
        .global idle_stack
        .lcomm idle_stack 128
