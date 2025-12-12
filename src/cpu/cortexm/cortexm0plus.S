        .syntax unified
        .text
        .thumb
        .thumb_func
        .global __sync_add_and_fetch_4
__sync_add_and_fetch_4:
        mrs r3, primask
        cpsid i
        ldr r2, [r0]
        add r2, r1
        str r2, [r0]
        msr primask, r3
        mov r0, r2
        bx lr


        .global mpu_disable
mpu_disable:
        bx lr

