        .syntax unified
        .thumb
        .section    .bootloader_vector,"a",%progbits
        .align      5       // 32-byte aligned (boot ROM requirement)
        .global     bl_start
        .thumb_func
bl_start:
        // Boot ROM passes the boot context address in r0 — it becomes the
        // argument to bl_main(ctx). Don't touch r0.

        // Stack: place MSP high in the AXISRAM2 download buffer, above our
        // loaded image (.boot + parked payload end ~0x341A_xxxx). bl_start
        // (0x34180400) would grow down into the ROM's header region at
        // 0x34180000, which the ROM keeps protected in serial boot.
        ldr r1, =0x341C0000
        msr msp, r1

        // Open debug access port so SWD can attach after the ROM branches
        // (BSEC_AP_UNLOCK = 0xB4, BSEC_DBGCR = 0xB451B400).
        ldr r1, = 0x56009E90
        mov r2, #0xB4
        str r2, [r1]
        ldr r1, = 0x56009E8C
        ldr r2, = 0xB451B400
        str r2, [r1]

        // Clear stack limits inherited from the ROM.
        mov r1, #0
        msr msplim, r1
        msr psplim, r1

        b bl_main
