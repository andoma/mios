/* Included by platform specific linker script */

SECTIONS {
        .isr_vector : {
                . = ALIGN(4);
                KEEP(*(.isr_vector))
                . = ALIGN(4);
        } >VTBL

        .text : {
                . = ALIGN(4);
                *(.text)
                *(.text.*)
                *(.rodata)
                *(.rodata.*)
        } >FLASH

        .inits : {
                . = ALIGN(4);
                _init_array_begin = .;
                *(.init_array)
                KEEP(*(SORT_BY_NAME(.init_array.*)))
                _init_array_end = .;

                . = ALIGN(4);
                _fini_array_begin = .;
                *(.fini_array)
                KEEP(*(SORT_BY_NAME(.fini_array.*)))
                _fini_array_end = .;

                . = ALIGN(4);
                _clicmd_array_begin = .;
                KEEP(*(clicmd))
                _clicmd_array_end = .;
                . = ALIGN(4);

                . = ALIGN(4);
                _netsock_array_begin = .;
                KEEP(*(netsock))
                _netsock_array_end = .;
                . = ALIGN(4);

                . = ALIGN(4);
                _rpcdef_array_begin = .;
                KEEP(*(rpcdef))
                _rpcdef_array_end = .;
                . = ALIGN(4);
        } >FLASH

        .build_id : {
                . = ALIGN(4);
                PROVIDE(_build_id = .);
                KEEP(*(.note.gnu.build-id))
        } >FLASH

        .ARM.exidx : {
                . = ALIGN(4);
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
                *(.bss.*)
                *(COMMON)
                . = ALIGN(16);
                _ebss = .;
        } >RAM
}
