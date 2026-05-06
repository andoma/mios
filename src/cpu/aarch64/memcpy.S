// memcpy(void *dst, const void *src, size_t n) -> void *
//   x0 = dst, x1 = src, x2 = n; returns dst.
//
// Strategy:
//   n == 0:        no-op
//   1..15:         byte loop
//   16..32:        ldp/stp head 16 + tail 16 (overlap when n < 32)
//   > 32:          ldp/stp head 32, 32-byte loop, ldp/stp tail 32
//                  (final 32 anchored at src_end overlaps last loop iter,
//                   safe for non-overlapping src/dst forward copy)
//
// Strong override of the weak C memcpy in lib/libc/string.c. Assumes
// unaligned LDP/STP is permitted by the active memory mapping (Normal,
// not Device); see entry.S MAIR setup.

        .section .text.memcpy, "ax", %progbits
        .global memcpy
        .type memcpy, %function

memcpy:
        mov     x6, x0                  // saved dst for return

        cmp     x2, #16
        b.lo    .Lmemcpy_lt16

        add     x4, x1, x2              // src_end
        add     x5, x0, x2              // dst_end

        cmp     x2, #32
        b.ls    .Lmemcpy_16_32

        // n > 32: head 32, then loop, then overlapping tail 32.
        ldp     x7, x8,  [x1]
        ldp     x9, x10, [x1, #16]
        stp     x7, x8,  [x0]
        stp     x9, x10, [x0, #16]

        add     x1, x1, #32
        add     x0, x0, #32
        sub     x2, x2, #32

        cmp     x2, #32
        b.ls    .Lmemcpy_tail32

.Lmemcpy_loop32:
        ldp     x7, x8,  [x1], #16
        ldp     x9, x10, [x1], #16
        stp     x7, x8,  [x0], #16
        stp     x9, x10, [x0], #16
        sub     x2, x2, #32
        cmp     x2, #32
        b.hi    .Lmemcpy_loop32

.Lmemcpy_tail32:
        ldp     x7, x8,  [x4, #-32]
        ldp     x9, x10, [x4, #-16]
        stp     x7, x8,  [x5, #-32]
        stp     x9, x10, [x5, #-16]
        mov     x0, x6
        ret

.Lmemcpy_16_32:
        ldp     x7, x8,  [x1]
        ldp     x9, x10, [x4, #-16]
        stp     x7, x8,  [x0]
        stp     x9, x10, [x5, #-16]
        mov     x0, x6
        ret

.Lmemcpy_lt16:
        cbz     x2, .Lmemcpy_done
.Lmemcpy_byte:
        ldrb    w3, [x1], #1
        strb    w3, [x0], #1
        subs    x2, x2, #1
        b.ne    .Lmemcpy_byte
.Lmemcpy_done:
        mov     x0, x6
        ret

        .size memcpy, . - memcpy
