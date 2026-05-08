/* SRAM2 and SRAM4 regions shared by the application and bootloader
 * linker scripts. Placement in these regions is opt-in via the .sram2
 * and .sram4 input sections. _esram2 / _esram4 mark the end of any
 * statically allocated content so the heap can be added above it.
 *
 * The last 512 bytes of SRAM4 are reserved for the crashlog buffer
 * (see CRASHLOG_ADDR in stm32h7.c) and excluded from the region.
 */

MEMORY {
         SRAM2 (xrw): ORIGIN = 0x30004000, LENGTH = 16K
         SRAM4 (xrw): ORIGIN = 0x38000000, LENGTH = 16K - 512
}

SECTIONS {
        .sram2 (NOLOAD) : {
                . = ALIGN(4);
                _ssram2 = .;
                KEEP(*(.sram2))
                KEEP(*(.sram2.*))
                . = ALIGN(4);
                _esram2 = .;
        } >SRAM2

        .sram4 (NOLOAD) : {
                . = ALIGN(4);
                _ssram4 = .;
                KEEP(*(.sram4))
                KEEP(*(.sram4.*))
                . = ALIGN(4);
                _esram4 = .;
        } >SRAM4
}
