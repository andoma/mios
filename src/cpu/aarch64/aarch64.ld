/* Included by platform specific linker script */

SECTIONS {

        .init (READONLY) : {
                . = ALIGN(4096);
                *(.init)
                . = ALIGN(4096);
        } >INIT

        .text (READONLY) : {
                . = ALIGN(4096);
                *(.text)
                *(.text.*)
                *(.rodata)
                *(.rodata.*)

                . = ALIGN(8);
                _init_array_begin = .;
                *(.init_array)
                KEEP(*(SORT_BY_NAME(.init_array.*)))
                _init_array_end = .;

                . = ALIGN(8);
                _fini_array_begin = .;
                *(.fini_array)
                KEEP(*(SORT_BY_NAME(.fini_array.*)))
                _fini_array_end = .;

                . = ALIGN(8);
                _clicmd_array_begin = .;
                KEEP(*(SORT_BY_NAME(clicmd.*)))
                _clicmd_array_end = .;

                . = ALIGN(8);
                _rpcdef_array_begin = .;
                KEEP(*(rpcdef))
                _rpcdef_array_end = .;

                . = ALIGN(8);
                _servicedef_array_begin = .;
                KEEP(*(servicedef))
                _servicedef_array_end = .;

                . = ALIGN(8);
                _httproute_array_begin = .;
                KEEP(*(httproute))
                _httproute_array_end = .;

                . = ALIGN(8);
                _udpinput_array_begin = .;
                KEEP(*(udpinput))
                _udpinput_array_end = .;

                . = ALIGN(8);
                _ghook_array_begin = .;
                KEEP(*(ghook))
                _ghook_array_end = .;
                . = ALIGN(8);

                . = ALIGN(8);
                _driver_array_begin = .;
                KEEP(*(SORT_BY_NAME(driver.*)))
                _driver_array_end = .;
                . = ALIGN(8);

                . = ALIGN(4096);
                _efi_runtime_begin = .;
                KEEP(*(efi_runtime))
                _efi_runtime_end = .;
                . = ALIGN(8);

        } >TEXT AT>INIT

        .build_id (READONLY) : {
                . = ALIGN(8);
                PROVIDE(_build_id = .);
                KEEP(*(.note.gnu.build-id))
                . = ALIGN(8);
        } >TEXT AT>INIT

        .miosversion (READONLY) : {
                . = ALIGN(8);
                PROVIDE(_miosversion = .);
                KEEP(*(miosversion))
                . = ALIGN(8);
        } >TEXT AT>INIT

        .appversion (READONLY) : {
                . = ALIGN(8);
                PROVIDE(_appversion = .);
                KEEP(*(appversion))
                . = ALIGN(8);
        } >TEXT AT>INIT

        .appname (READONLY) : {
                . = ALIGN(8);
                PROVIDE(_appname = .);
                KEEP(*(appname))
                . = ALIGN(8);
        } >TEXT AT>INIT

        .data (READONLY) : {
                . = ALIGN(8);
                _sdata = .;
                *(.data);
                *(.data.*);
                _edata = .;
                . = ALIGN(8);
        } >TEXT AT>INIT

        .mmusetup (READONLY) : {
                . = ALIGN(8);
                 *(.mmusetup)
        } >INIT


        .stack (NOLOAD): {
                . = ALIGN(4096);
                _sp1_start = .;
                . = . + 16384;
                _sp1_end = .;
        } >BSS

        .bss (NOLOAD): {
                . = ALIGN(4096);
                _sbss = .;
                *(.bss)
                *(.bss.*)
                _ebss = .;
                . = ALIGN(4096);
        } >BSS

}


