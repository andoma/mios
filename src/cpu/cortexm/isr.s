        .syntax unified
        .text
        .thumb
        .thumb_func

start:
//        cpsie i     // Disable interrupts, later enabled in irq_init()
        bl init
        mov r0, #2  // Threaded mode
        msr control, r0
        isb
        ldr r0, =idle_stack + 64
        msr psp, r0
        isb
        mov r12, #1 // SYS_relinquish
        svc #0
idle:   wfi
        b idle

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
        ldr pc, [r12, #1024]

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
        .long syscall

        .long exc_reserved
        .long exc_reserved
        .long pendsv
        .long exc_systick

        .set i,0
        .rept 240
        insert_irq %i
        .set i, i+1
        .endr

syscall_table:
        .long sys_yield
        .long sys_relinquish
        .long sys_task_start
        .long sys_sleep

        .size vectors, . - vectors

        .bss
        .align 8
        .lcomm idle_stack 64
