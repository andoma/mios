/* nRF54L15: 1524 kB RRAM @ 0x00000000, 256 kB RAM @ 0x20000000 (contiguous) */

MEMORY {
         /* Vector table: 16 + CORTEXM_IRQ_COUNT(240) = 256 entries = 0x400 */
         VTBL  (rx) : ORIGIN = 0x00000000, LENGTH = 0x400
         FLASH (rx) : ORIGIN = 0x00000400, LENGTH = 0x17CC00
         /* First 0x400 of RAM is the boot/exception (MSP) stack */
         RAM   (xrw): ORIGIN = 0x20000400, LENGTH = 0x3FC00
}

INCLUDE cpu/cortexm/cortexm.ld
