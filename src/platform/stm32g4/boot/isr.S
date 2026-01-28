        .syntax unified
        .text
        .thumb
        .section    .bootloader_vector,"a",%progbits
        .align      2
        .global     bootloader_vectors
        .type   bootloader_vectors, %object



bootloader_vectors:
        .long _sdata          // Main stack (MSP) start before data-section
        .long bl_start
        .long bl_nmi
        .long bl_fault
        .long bl_fault
        .long bl_fault
        .long bl_fault
        .long 0xb00710ad  // Magic version cookie
        .long 1           // Version

        .size bootloader_vectors, . - bootloader_vectors

