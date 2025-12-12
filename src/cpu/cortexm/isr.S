        .syntax unified
        .text
        .thumb
        .thumb_func
        .global start
start:
        cpsid i

        // Fill MSP stack with poison value
        ldr r0,=_sdata
        ldr r1,=#0x20000000
        ldr r2,=#0xdeadcafe
1:      str r2, [r1]
        adds r1, #4
        cmp r0, r1
        bne 1b
        mov sp, r0

        // Copy rw-data segment from Flash to RAM
        ldr r1,=_etext
        ldr r2,=_edata
        subs r2, r2, r0
        bl memcpy

        // All initialization in C-land
        bl init

        movs r0, #0
        subs r0, #1
        mov lr, r0
        // Switch to PSP and start multitasking
        movs r0, #2
        msr control, r0
        isb
        cpsie i     // Enable interrupts, off we go
        isb
        ldr r0,=cpu_idle
        bx  r0

        .thumb_func
pendsv:
#ifdef __thumb2__
        .fnstart
        .cantunwind
#endif
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
#ifdef __thumb2__
        .fnend
#endif
        .global cpu_softreset
        .thumb_func
cpu_softreset:
        movs r1, #0
        msr control, r1
        msr primask, r1
        msr faultmask, r1
        msr basepri, r1
        isb
        ldr r1, [r0]
        ldr r2, [r0, #4]
        msr msp, r1
        bx r2

#ifdef __thumb2__
        .global backtrace_print
        .type   backtrace_print, %function

backtrace_print:
        .fnstart
        mov     r1, sp
        push    {lr}
        push    {lr}
        push    {lr}
        push    {r1}
        push    {r0-r12}
        mov     r1, r10
        mov     r2, r11
        mov     r3, r12
        push    {r1-r3}
        mov     r1, r9
        mov     r2, r8
        push    {r1-r2}
        push    {r0-r7}
        mov     r1, sp
        mrs     r2, ipsr
        bl      backtrace_regs

        add     sp, sp, #64
        pop     {pc}
        .fnend
#endif

/*
 * Helper to generate a partial register-context
 * We load it up enough so the code in unwind.c will do the correct
 * thing when it detects an EXC_RETURN in lr
 */

        .thumb_func
exc_common_fault:
        mov     r1, sp
        push    {lr}
        push    {lr}
        push    {lr}
        push    {r1}

#ifdef __thumb2__
        push    {r0-r12}
#else
        mov     r1, r10
        mov     r2, r11
        mov     r3, r12
        push    {r1-r3}
        mov     r1, r9
        mov     r2, r8
        push    {r1-r2}
        push    {r0-r7}
#endif
        mov     r1, r0
        mov     r0, sp
        bx      r1

        .thumb_func
exc_nmi0:
        ldr r0,=exc_nmi
        b exc_common_fault

        .thumb_func
exc_hard_fault0:
        ldr r0,=exc_hard_fault
        b exc_common_fault

        .thumb_func
exc_mm_fault0:
        ldr r0,=exc_mm_fault
        b exc_common_fault

        .thumb_func
exc_bus_fault0:
        ldr r0,=exc_bus_fault
        b exc_common_fault

        .thumb_func
exc_usage_fault0:
        push {lr}
        bl  exc_handle_usage_fault
        cmp     r0, #0
        beq     1f

        ldr r0,=exc_usage_fault
        b exc_common_fault
1:      pop  {pc}


// ----------- Vectors ------------------------------------------

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
        .long exc_nmi0
        .long exc_hard_fault0

        .long exc_mm_fault0
        .long exc_bus_fault0
        .long exc_usage_fault0
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
