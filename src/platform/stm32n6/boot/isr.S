        .syntax unified
        .thumb
        .section    .bootloader_vector,"a",%progbits
        .align      5       // 32-byte aligned (boot ROM requirement)
        .global     bl_start
        .thumb_func
bl_start:
        // Boot ROM passes boot context address in r0.
        // Don't touch r0 — it becomes the argument to bl_main(ctx).

        // Set MSP to just below our code (stack grows down)
        ldr r1, =bl_start
        msr msp, r1

        // Open debug access port (BSEC_AP_UNLOCK = 0xB4)
        ldr r1, = 0x56009E90
        mov r2, #0xB4
        str r2, [r1]

        // Enable non-secure/secure debug (BSEC_DBGCR = 0xB451B400)
        ldr r1, = 0x56009E8C
        ldr r2, = 0xB451B400
        str r2, [r1]

        // Clear stack limits
        mov r1, #0
        msr msplim, r1
        msr psplim, r1

        b bl_main
