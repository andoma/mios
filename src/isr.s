        .syntax unified
        .text
        .global exc_svc_thunk
        .thumb
        .thumb_func

exc_svc_thunk:
        push {lr}
        mrs r12, psp
        stmdb r12!, {r4-r11}
        msr psp, r12
        bl svc_handler
        pop {lr}
        mrs r12, psp
        ldmfd r12!, {r4-r11}
        msr psp, r12
        bx lr

        .section    .isr_vector,"aw",%progbits
        .align      2
        .global     vectors
        .type   vectors, %object

vectors:
        .long 0x20001000
        .long init
        .long exc_nmi
        .long exc_hard_fault
        .long exc_mm_fault
        .long exc_bus_fault
        .long exc_usage_fault
        .long exc_reserved
        .long exc_reserved
        .long exc_reserved
        .long exc_reserved
        .long exc_svc_thunk
        .long exc_reserved
        .long exc_reserved
        .long exc_pendsv
        .long exc_systick
        .size vectors, . - vectors
