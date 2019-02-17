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
	addi sp, sp, -256
        sd  x1,  1 * 8(sp)
#        sd  x2,  2 * 8(sp)
        sd  x3,  3 * 8(sp)
        sd  x4,  4 * 8(sp)
        sd  x5,  5 * 8(sp)
        sd  x6,  6 * 8(sp)
        sd  x7,  7 * 8(sp)
        sd  x8,  8 * 8(sp)
        sd  x9,  9 * 8(sp)
        sd x10, 10 * 8(sp)
        sd x11, 11 * 8(sp)
        sd x12, 12 * 8(sp)
        sd x13, 13 * 8(sp)
        sd x14, 14 * 8(sp)
        sd x15, 15 * 8(sp)
        sd x16, 16 * 8(sp)
        sd x17, 17 * 8(sp)
        sd x18, 18 * 8(sp)
        sd x19, 19 * 8(sp)
        sd x20, 20 * 8(sp)
        sd x21, 21 * 8(sp)
        sd x22, 22 * 8(sp)
        sd x23, 23 * 8(sp)
        sd x24, 24 * 8(sp)
        sd x25, 25 * 8(sp)
        sd x26, 26 * 8(sp)
        sd x27, 27 * 8(sp)
        sd x28, 28 * 8(sp)
        sd x29, 29 * 8(sp)
        sd x30, 30 * 8(sp)
        sd x31, 31 * 8(sp)
        csrr a0, mcause
	csrr a1, mepc
	csrr a2, mtval

	jal handle_trap

        beq zero, a0, 1f

	csrr a0, mepc
        sd   a0, (sp)
        mv   a0, sp

        jal  sys_switch

        mv   sp, a0
        ld   a0, (sp)
        csrw mepc, a0

1:
        ld  x1,  1 * 8(sp)
#        ld  x2,  2 * 8(sp)
        ld  x3,  3 * 8(sp)
        ld  x4,  4 * 8(sp)
        ld  x5,  5 * 8(sp)
        ld  x6,  6 * 8(sp)
        ld  x7,  7 * 8(sp)
        ld  x8,  8 * 8(sp)
        ld  x9,  9 * 8(sp)
        ld x10, 10 * 8(sp)
        ld x11, 11 * 8(sp)
        ld x12, 12 * 8(sp)
        ld x13, 13 * 8(sp)
        ld x14, 14 * 8(sp)
        ld x15, 15 * 8(sp)
        ld x16, 16 * 8(sp)
        ld x17, 17 * 8(sp)
        ld x18, 18 * 8(sp)
        ld x19, 19 * 8(sp)
        ld x20, 20 * 8(sp)
        ld x21, 21 * 8(sp)
        ld x22, 22 * 8(sp)
        ld x23, 23 * 8(sp)
        ld x24, 24 * 8(sp)
        ld x25, 25 * 8(sp)
        ld x26, 26 * 8(sp)
        ld x27, 27 * 8(sp)
        ld x28, 28 * 8(sp)
        ld x29, 29 * 8(sp)
        ld x30, 30 * 8(sp)
        ld x31, 31 * 8(sp)
	addi sp, sp, 256
        mret
