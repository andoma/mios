    .text
    .global start
start:

    ldr x30, =0x40100000
    mov sp, x30

    // Copy rw-data segment from Flash to RAM
    ldr x0,=_sdata
    ldr x1,=_etext
    ldr x2,=_edata
    subs x2, x2, x0
    bl memcpy

    bl init

1:  wfi
    b 1b

    .section    .vectortable,"a",%progbits
    .align 8
    .global vectors
    .type   vectors, %object

    // curr_el_sp0_sync
    .balign 0x800
    bl curr_el_sp0_sync
    eret
.pool
    // curr_el_sp0_irq
    .balign 0x80
    bl curr_el_sp0_irq
    eret
.pool

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

