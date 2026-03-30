        .syntax unified
        .thumb
        .section    .bootloader_vector,"a",%progbits
        .align      5       // 32-byte aligned (boot ROM requirement)
        .global     bl_start
        .thumb_func
bl_start:
        // Boot ROM jumps here. MSP points to boot ROM's stack in AXISRAM2.
        // Just use it as-is. Skip DTCM for now.

        adr r0, #bl_start
        msr msp, r0

        // Open debug access port
        ldr r0, = 0x56009E90
        mov r1, #0xB4
        str r1, [r0]

        // Enable non-secure/secure debug
        ldr r0, = 0x56009E8C
        ldr r1, = 0xB451B400
        str r1, [r0]

        // Clear stack limits
        movs r0, #0
        msr msplim, r0
        msr psplim, r0

        b bl_main
