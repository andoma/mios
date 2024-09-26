/* Included by platform specific linker script */

SECTIONS {
        .text : {
                . = ALIGN(4);
                KEEP(*(.vector_table))
                *(.text)
                *(.text.*)
                *(.rodata)
                *(.rodata.*)

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
                _rpcdef_array_begin = .;
                KEEP(*(rpcdef))
                _rpcdef_array_end = .;
                . = ALIGN(4);

                . = ALIGN(4);
                _servicedef_array_begin = .;
                KEEP(*(servicedef))
                _servicedef_array_end = .;
                . = ALIGN(4);

                . = ALIGN(4);
                _httproute_array_begin = .;
                KEEP(*(httproute))
                _httproute_array_end = .;
                . = ALIGN(4);

                . = ALIGN(4);
                _udpinput_array_begin = .;
                KEEP(*(udpinput))
                _udpinput_array_end = .;
                . = ALIGN(4);

                . = ALIGN(4);
                _ghook_array_begin = .;
                KEEP(*(ghook))
                _ghook_array_end = .;
                . = ALIGN(4);
        } >RAM

        .build_id : {
                . = ALIGN(4);
                PROVIDE(_build_id = .);
                KEEP(*(.note.gnu.build-id))
        } >RAM

        .miosversion : {
                . = ALIGN(4);
                PROVIDE(_miosversion = .);
                KEEP(*(miosversion))
        } >RAM

        .appversion : {
                . = ALIGN(4);
                PROVIDE(_appversion = .);
                KEEP(*(appversion))
        } >RAM

        .appname : {
                . = ALIGN(4);
                PROVIDE(_appname = .);
                KEEP(*(appname))
                . = ALIGN(8);
        } >RAM

        .data : {
                . = ALIGN(8);
                _sdata = .;
                *(.data)
                *(.data.*)
                . = ALIGN(4);
                _edata = .;
        } >RAM

        .stack (NOLOAD): {
                . = ALIGN(8);
                _svcstack_start = .;
                . = . + 4096;
                _svcstack_end = .;
                _irqstack_start = .;
                . = . + 4096;
                _irqstack_end = .;
                _abtstack_start = .;
                . = . + 4096;
                _abtstack_end = .;
        } >RAM

        .bss (NOLOAD): {
                . = ALIGN(4);
                _sbss = .;
                *(.bss)
                *(.bss.*)
                *(COMMON)
                . = ALIGN(16);
                _ebss = .;
        } >RAM


}
