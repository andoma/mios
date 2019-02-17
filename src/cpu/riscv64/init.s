        .globl _start
_start:
        la x5, trap
        csrw mtvec, x5
        fence
        la sp, _sstack
        jal ra, init

        # Enable SW interrupt

        li x5, 0x8
        csrs mie, x5

        # Enable interrupts

        li x5, 0x8
        csrs mstatus, x5

idle:
        wfi
        j idle

        .align 4
trap:
	addi sp, sp, -8 * 16
        sd  x1,  0 * 8(sp)
        sd  x5,  1 * 8(sp)
        sd  x6,  2 * 8(sp)
        sd  x7,  3 * 8(sp)
        sd x10,  4 * 8(sp)
        sd x11,  5 * 8(sp)
        sd x12,  6 * 8(sp)
        sd x13,  7 * 8(sp)
        sd x14,  8 * 8(sp)
        sd x15,  9 * 8(sp)
        sd x16, 10 * 8(sp)
        sd x17, 11 * 8(sp)
        sd x28, 12 * 8(sp)
        sd x29, 13 * 8(sp)
        sd x30, 14 * 8(sp)
        sd x31, 15 * 8(sp)

        csrr a0, mcause

	jal handle_trap

        beq zero, a0, 1f

      	addi sp, sp, -8 * 14

	csrr a0, mepc
        sd  a0,  0 * 8(sp)
        sd  x4,  1 * 8(sp)
        sd  x8,  2 * 8(sp)
        sd  x9,  3 * 8(sp)
        sd x18,  4 * 8(sp)
        sd x19,  5 * 8(sp)
        sd x20,  6 * 8(sp)
        sd x21,  7 * 8(sp)
        sd x22,  8 * 8(sp)
        sd x23,  9 * 8(sp)
        sd x24, 10 * 8(sp)
        sd x25, 11 * 8(sp)
        sd x26, 12 * 8(sp)
        sd x27, 13 * 8(sp)

        mv   a0, sp
        jal  sys_switch
        mv   sp, a0

        ld  a0,  0 * 8(sp)
        csrw mepc, a0
        ld  x4,  1 * 8(sp)
        ld  x8,  2 * 8(sp)
        ld  x9,  3 * 8(sp)
        ld x18,  4 * 8(sp)
        ld x19,  5 * 8(sp)
        ld x20,  6 * 8(sp)
        ld x21,  7 * 8(sp)
        ld x22,  8 * 8(sp)
        ld x23,  9 * 8(sp)
        ld x24, 10 * 8(sp)
        ld x25, 11 * 8(sp)
        ld x26, 12 * 8(sp)
        ld x27, 13 * 8(sp)

      	addi sp, sp, 8 * 14
1:
        ld  x1,  0 * 8(sp)
        ld  x5,  1 * 8(sp)
        ld  x6,  2 * 8(sp)
        ld  x7,  3 * 8(sp)
        ld x10,  4 * 8(sp)
        ld x11,  5 * 8(sp)
        ld x12,  6 * 8(sp)
        ld x13,  7 * 8(sp)
        ld x14,  8 * 8(sp)
        ld x15,  9 * 8(sp)
        ld x16, 10 * 8(sp)
        ld x17, 11 * 8(sp)
        ld x28, 12 * 8(sp)
        ld x29, 13 * 8(sp)
        ld x30, 14 * 8(sp)
        ld x31, 15 * 8(sp)

        addi sp, sp, 128
        mret
