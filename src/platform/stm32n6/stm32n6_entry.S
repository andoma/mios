        .syntax unified
        .text
        .thumb
        .thumb_func
        .global start
start:
        cpsid i

        // Clear stack limits so the zero-fill loop doesn't trip
        ldr r0,=#0x0
        msr msplim, r0
        msr psplim, r0

        // Fill entire DTCM (128 KB) with poison value to initialize
        // ECC syndrome bits. After reset, DTCM ECC is indeterminate --
        // any read of an unwritten location will generate a bus fault.
        ldr r0,=#0x20020000
        ldr r1,=#0x20000000
        ldr r2,=#0xdeadcafe
1:      str r2, [r1]
        adds r1, #4
        cmp r0, r1
        bne 1b

        // Set up MSP in DTCM
        ldr r0,=#0x20000400
        mov sp, r0

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
