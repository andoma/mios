/* nRF52840 Dongle (PCA10059). The factory Nordic MBR occupies the first 4 kB
 * and the USB DFU bootloader lives at the top of flash (0xE0000+), so the
 * application starts at 0x1000. mios sets VTOR to the .isr_vector below during
 * early init (cpu/cortexm/irq.c), so the relocated table just works.
 *
 * Flash: MBR 0x0..0x1000, app 0x1000..0xE0000, bootloader+settings above.
 * RAM: 256 kB; the bottom 0x200 is left for the initial/exception stack.
 */
MEMORY {
         VTBL  (rx) : ORIGIN = 0x00001000, LENGTH = 256
         FLASH (rx) : ORIGIN = 0x00001100, LENGTH = 0xDEF00
         RAM   (xrw): ORIGIN = 0x20000200, LENGTH = 0x3FE00
}

INCLUDE cpu/cortexm/cortexm.ld
