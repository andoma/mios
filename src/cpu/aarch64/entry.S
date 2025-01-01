        .text
        .global start
start:

        ldr x30, =_sp1_end
        mov sp, x30

        bl init

        // Switch to SP0
        msr spsel, #0
        isb
        // Enable interrupts
        msr daifclr, #2
1:      wfi
        b 1b
        .pool

        .global vectors
        .balign 0x800
vectors:
        // curr_el_sp0_sync
        bl curr_el_sp0_sync
        eret
        .pool

        // curr_el_sp0_irq
        .balign 0x80
        b curr_el_sp0_irq

        // curr_el_sp0_fiq
        .balign 0x80
        bl curr_el_sp0_fiq
        eret
        .pool

        // curr_el_sp0_serror
        .balign 0x80
        bl curr_el_sp0_serror
        eret
        .pool

        // curr_el_spx_sync
        .balign 0x80
        bl curr_el_spx_sync
        eret
        .pool

        // curr_el_spx_irq
        .balign 0x80
        bl curr_el_spx_irq
        eret
        .pool

        // curr_el_spx_fiq
        .balign 0x80
        bl curr_el_spx_fiq
        eret
        .pool

        // curr_el_spx_serror
        .balign 0x80
        bl curr_el_spx_serror
        eret
        .pool

        // lower_el_aarch64_sync
        .balign 0x80
        bl curr_el_aarch64_sync
        eret
        .pool

        // lower_el_aarch64_irq
        .balign 0x80
        bl curr_el_aarch64_irq
        eret
        .pool

        // lower_el_aarch64_fiq
        .balign 0x80
        bl curr_el_aarch64_fiq
        eret
        .pool

        // lower_el_aarch64_serror
        .balign 0x80
        bl lower_el_aarch64_serror
        eret
        .pool

        // lower_el_aarch32_sync
        .balign 0x80
        bl lower_el_aarch32_sync
        eret
        .pool

        // lower_el_aarch32_irq
        .balign 0x80
        bl lower_el_aarch32_irq
        eret
        .pool

        // lower_el_aarch32_fiq
        .balign 0x80
        bl lower_el_aarch32_fiq
        eret
        .pool

        // lower_el_aarch32_serror
        .balign 0x80
        bl lower_el_aarch32_serror
        eret
        .pool

        .balign 0x80

curr_el_sp0_irq:
        stp x20, x21, [sp, #-16]!
        mrs x20, spsr_el1
        mrs x21, elr_el1

        stp x22, x23, [sp, #-16]!
        mrs x22, sp_el0
        mrs x23, icc_iar1_el1

//        msr daifclr, #2         // Reenable interrupts

        cbz x23, cpu_task_switch // IRQ 0 (SGI) is task switch

        stp x0, x1, [sp, #-16]!
        stp x2, x3, [sp, #-16]!
        stp x4, x5, [sp, #-16]!
        stp x6, x7, [sp, #-16]!
        stp x8, x9, [sp, #-16]!
        stp x10, x11, [sp, #-16]!
        stp x12, x13, [sp, #-16]!
        stp x14, x15, [sp, #-16]!
        stp x16, x17, [sp, #-16]!
        stp x18, x30, [sp, #-16]!

        mov x0, x23
        bl trap_irq

        ldp x18, x30, [sp], #16
        ldp x16, x17, [sp], #16
        ldp x14, x15, [sp], #16
        ldp x12, x13, [sp], #16
        ldp x10, x11, [sp], #16
        ldp x8, x9, [sp], #16
        ldp x6, x7, [sp], #16
        ldp x4, x5, [sp], #16
        ldp x2, x3, [sp], #16
        ldp x0, x1, [sp], #16

        msr icc_eoir1_el1, x23
        ldp x22, x23, [sp], #16

//      msr daifset, #2 // Disable interrupts

        msr spsr_el1, x20
        msr elr_el1, x21

        ldp x20, x21, [sp], #16
        eret

cpu_task_switch:

        // Save user context

        stp x20, x21, [x22, #-16]!         // SPSR, ELR
        stp x0, x1, [x22, #-16]!
        stp x2, x3, [x22, #-16]!
        stp x4, x5, [x22, #-16]!
        stp x6, x7, [x22, #-16]!
        stp x8, x9, [x22, #-16]!
        stp x10, x11, [x22, #-16]!
        stp x12, x13, [x22, #-16]!
        stp x14, x15, [x22, #-16]!
        stp x16, x17, [x22, #-16]!
        stp x18, x19, [x22, #-16]!
        ldp x0, x1, [sp, #16]               // original x20, x21
        stp x0, x1, [x22, #-16]!
        ldp x2, x3, [sp, #0]                // original x22, x23
        stp x2, x3, [x22, #-16]!
        stp x24, x25, [x22, #-16]!
        stp x26, x27, [x22, #-16]!
        stp x28, x29, [x22, #-16]!
        str x30, [x22, #-16]!

        mov x0, x22
        bl task_switch
        mov x22, x0

        ldr x30,      [x22], #16
        ldp x28, x29, [x22], #16
        ldp x26, x27, [x22], #16
        ldp x24, x25, [x22], #16

        ldp x2, x3, [x22], #16
        stp x2, x3, [sp, #0]                // original x22, x23
        ldp x0, x1, [x22], #16
        stp x0, x1, [sp, #16]               // original x20, x21

        ldp x18, x19, [x22], #16
        ldp x16, x17, [x22], #16
        ldp x14, x15, [x22], #16
        ldp x12, x13, [x22], #16
        ldp x10, x11, [x22], #16
        ldp x8, x9, [x22], #16
        ldp x6, x7, [x22], #16
        ldp x4, x5, [x22], #16
        ldp x2, x3, [x22], #16
        ldp x0, x1, [x22], #16
        ldp x20, x21, [x22], #16

        msr sp_el0, x22
        msr icc_eoir1_el1, x23
        ldp x22, x23, [sp], #16

//        msr daifset, #2 // Disable interrupts

        msr spsr_el1, x20
        msr elr_el1, x21
        ldp x20, x21, [sp], #16
        eret

        .pool
