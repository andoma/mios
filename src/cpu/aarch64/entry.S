        .section .init,"ax",%progbits
        .global start
start:
        adr x16, .      // Keep track of load address

        mrs x0, currentel
        cmp x0, #4
        b.eq el1
        // Drop to EL1

        mov x0, (1 << 31)
        msr hcr_el2, x0

        adr x0, el1
        msr elr_el2, x0

        ldr x0, =0x000003c5
        msr spsr_el2, x0

        eret
        .pool

el1:
        bl mmusetup
        ldr x0, =vstart
        br x0
        .pool

        .text
        .global vstart
vstart:
        adr x0, load_addr
        str x16, [x0]
        adr x0, fdt_addr
        str x7, [x0]

        adr x0, vectors
        msr vbar_el1, x0

        ldr x0, =_sp1_end
        mov sp, x0

        bl init

        // Switch to SP0
        msr spsel, #0
        isb
        // Enable interrupts
        msr daifclr, #2
        // Enable async aborts
        msr daifclr, #4
1:      wfi
        b 1b
        .pool

        .global load_addr
load_addr:
        .xword 0
        .global fdt_addr
fdt_addr:
        .xword 0


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


        // MMU setup

        .section .mmusetup,"ax",%progbits
        .global mmusetup
mmusetup:

        adr x8, binary_end
        mov x7, x8

        // Check and skip FDT
        ldr w0, [x8]
        ldr w1, =0xedfe0dd0
        cmp w0, w1
        bne 1f
        ldr w0, [x8, #4]
        rev w0, w0
        add x8, x8, w0, uxtw

1:
        add x8, x8, #4095
        and x8, x8, #~4095

        mov x0, x8
        add x1, x0, 16384
1:      str xzr, [x0], #8
        cmp x1, x0
        bne 1b

        .global mmuinit
mmuinit:
        // MAIR 0 - Device-nGnRnE memory
        // MAIR 1 - Normal Memory
        ldr x1, =0xff00
        msr mair_el1, x1

        // 4kb page size, etc
        ldr x1, =0x5b510351b
        msr tcr_el1, x1
        isb

        // Identity map bottom 512GB
        mov x0, #0b11100100001
        mov x9, x8
        msr ttbr0_el1, x9

        add x2, x8, 4096
        mov x1, #(1 << 30)
1:
        str x0, [x8], #8
        add x0, x0, x1
        cmp x2, x8
        bne 1b

        add x10, x9, 4096    // x10 pagetable_v0
        msr ttbr1_el1, x10

        add x11, x10, 4096   // x11 pagetable_v1
        add x12, x11, 4096   // x12 pagetable_v2

        orr x0, x11, 3
        str x0, [x10]

        orr x0, x12, 3
        str x0, [x11]

        mov x1, #0b11100100101
        orr x1, x1, x16
        str x1, [x12]
        msr ttbr1_el1, x10

        add x17, x16, #2097152
        mov x1, #0b11100100101
        orr x1, x1, x17
        str x1, [x12, #8]

        dsb sy
        isb
        tlbi vmalle1

        mrs x0, sctlr_el1
        orr x0, x0, (1 << 0)    // MMU
        orr x0, x0, (1 << 2)    // D-Cache
        orr x0, x0, (1 << 12)   // I-Cache
        msr sctlr_el1, x0

        isb
        ret
        .pool
        .global binary_end
binary_end:
