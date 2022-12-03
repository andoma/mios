        .syntax unified
        .text
        .thumb
        .thumb_func
        .global start
start:
        ldr r0,=_sdata
        mov sp, r0
        cpsid i     // Disable interrupts
        bl init
        movs r0, #2  // Threaded mode
        msr control, r0
        isb
        cpsie i     // Enable interrupts, off we go
        isb
        ldr r0,=cpu_idle
        bx  r0

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

        .global cpu_softreset
cpu_softreset:
        movs r0, #0
        msr control, r0
        msr primask, r0
        msr faultmask, r0
        msr basepri, r0
        isb
        ldr r1, [r0]
        ldr r2, [r0, #4]
        msr msp, r1
        bx r2

        .section    .isr_vector,"a",%progbits
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
        .rept CORTEXM_IRQ_COUNT
        insert_irq %i
        .set i, i+1
        .endr

        .size vectors, . - vectors
