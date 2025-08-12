        .section .init,"ax",%progbits
        .global putc
putc:
        orr w0, w0, #0x80000000
        orr w0, w0, #0x01000000
        ldr x1, =0x0c168000
1:      ldr     w2, [x1]
        cbnz    w2, 1b
        str     w0, [x1]
        ret
