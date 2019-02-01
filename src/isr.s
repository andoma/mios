        .syntax unified
        .text
        .thumb
        .thumb_func

init0:
        bl init
        b multitask

        .thumb_func
pendsv:
        push {lr}
        mrs r0, psp
        isb
        stmdb r0!, {r4-r11}
        bl sys_switch
        ldmfd r0!, {r4-r11}
        msr psp, r0
        isb
        pop {pc}


        .thumb_func
syscall:
        lsl r12, r12, 2
        ldr pc, [r12, #0x40]

        .section    .isr_vector,"aw",%progbits
        .align      2
        .global     vectors
        .type   vectors, %object

vectors:
        .long _sdata          // Main stack (MSP) start before data-section
        .long init0
        .long exc_nmi
        .long exc_hard_fault

        .long exc_mm_fault
        .long exc_bus_fault
        .long exc_usage_fault
        .long exc_reserved

        .long exc_reserved
        .long exc_reserved
        .long exc_reserved
        .long syscall

        .long exc_reserved
        .long exc_reserved
        .long pendsv
        .long exc_systick

syscall_table:
        .long sys_yield
        .long sys_relinquish
        .long sys_task_start
        .long sys_sleep

        .size vectors, . - vectors
