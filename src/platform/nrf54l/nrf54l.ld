/* nRF54L15: 1524 kB RRAM @ 0x00000000, 256 kB RAM @ 0x20000000 (contiguous) */

MEMORY {
         /* Vector table: 16 + CORTEXM_IRQ_COUNT(270) = 286 entries = 0x478;
            rounded up to 0x800 (also satisfies VTOR power-of-two alignment) */
         VTBL  (rx) : ORIGIN = 0x00000000, LENGTH = 0x800
         FLASH (rx) : ORIGIN = 0x00000800, LENGTH = 0x17C800
         /* First 0x400 of RAM is the boot/exception (MSP) stack */
         RAM   (xrw): ORIGIN = 0x20000400, LENGTH = 0x3FC00
}

INCLUDE cpu/cortexm/cortexm.ld
