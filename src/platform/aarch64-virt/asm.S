        .section .init,"ax",%progbits
        .global putc
putc:
        ldr     x1, =0x09000000
1:      ldr     w2, [x1, #0x18]
        tbnz    w2, #5, 1b
        str     x0, [x1]
        ret

	.globl	semihosting_call
semihosting_call:
	hlt	#0xf000
	ret
