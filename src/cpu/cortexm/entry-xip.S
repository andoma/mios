        .syntax unified
        .text
        .thumb
        .thumb_func
        .global start
start:
        cpsid i

        // Fill MSP stack with poison value
        ldr r0,=_sdata
        ldr r1,=#0x20000000
        ldr r2,=#0xdeadcafe
1:      str r2, [r1]
        adds r1, #4
        cmp r0, r1
        bne 1b
        mov sp, r0

        // Copy rw-data segment from Flash to RAM
        ldr r1,=_etext
        ldr r2,=_edata
        subs r2, r2, r0
        bl memcpy

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
