/* Included by platform specific linker script */

SECTIONS {

        .text (READONLY) : {
                . = ALIGN(8);
                *(.text)
                *(.text.*)
                *(.rodata)
                *(.rodata.*)
        } >TEXT

        .inits (READONLY) : {
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
                _rpcdef_array_begin = .;
                KEEP(*(rpcdef))
                _rpcdef_array_end = .;

                . = ALIGN(4);
                _servicedef_array_begin = .;
                KEEP(*(servicedef))
                _servicedef_array_end = .;

                . = ALIGN(4);
                _httproute_array_begin = .;
                KEEP(*(httproute))
                _httproute_array_end = .;

                . = ALIGN(4);
                _udpinput_array_begin = .;
                KEEP(*(udpinput))
                _udpinput_array_end = .;

                . = ALIGN(4);
                _ghook_array_begin = .;
                KEEP(*(ghook))
                _ghook_array_end = .;
        } >TEXT

        .build_id (READONLY) : {
                . = ALIGN(4);
                PROVIDE(_build_id = .);
                KEEP(*(.note.gnu.build-id))
        } >TEXT

        .miosversion (READONLY) : {
                . = ALIGN(4);
                PROVIDE(_miosversion = .);
                KEEP(*(miosversion))
        } >TEXT

        .appversion (READONLY) : {
                . = ALIGN(4);
                PROVIDE(_appversion = .);
                KEEP(*(appversion))
        } >TEXT

        .appname (READONLY) : {
                . = ALIGN(4);
                PROVIDE(_appname = .);
                KEEP(*(appname))
                . = ALIGN(8);
                 _etext = .;
        } >TEXT

        .data : {
                . = ALIGN(8);
                _sdata = .;
                *(.data)
                *(.data.*)
                . = ALIGN(4);
                _edata = .;
        } >DATA

        .stack (NOLOAD): {
                . = ALIGN(16);
                _sp1_start = .;
                . = . + 16384;
                _sp1_end = .;
        } >DATA

        .bss (NOLOAD) : {
                . = ALIGN(4);
                _sbss = .;
                *(.bss)
                *(.bss.*)
                *(COMMON)
                . = ALIGN(16);
                _ebss = .;
        } >DATA
}


