MEMORY {
         FLASH (rx) : ORIGIN = 0x08000000, LENGTH = 512K
         CCM   (xrw): ORIGIN = 0x10000000, LENGTH = 64K
         RAM   (xrw): ORIGIN = 0x20000200, LENGTH = 192K
}

SECTIONS {
        .isr_vector : {
                . = ALIGN(4);
                KEEP(*(.isr_vector))
                . = ALIGN(4);
        } >FLASH

        .text : {
                . = ALIGN(4);
                *(.text)
                *(.text.*)
                *(.rodata)
                *(.rodata*)
                . = ALIGN(4);
                _init_array_begin = .;
                *(.init_array)
                *(SORT_BY_NAME(.init_array.*))
                _init_array_end = .;

                . = ALIGN(4);
                _clicmd_array_begin = .;
                *(clicmd)
                _clicmd_array_end = .;
                . = ALIGN(4);
        } >FLASH

        .ARM.exidx : {
                 *(.ARM.exidx*)
                 *(.gnu.linkonce.armexidx.*)
                 _etext = .;
        } >FLASH

        .data : AT ( _etext ) {
                . = ALIGN(4);
                _sdata = .;
                *(ramcode)
                *(.data)
                *(.data.*)
                . = ALIGN(4);
                _edata = .;
        } >RAM

        .bss : {
                . = ALIGN(4);
                _sbss = .;
                *(.bss)
                *(COMMON)
                . = ALIGN(16);
                _ebss = .;
        } >RAM
}
