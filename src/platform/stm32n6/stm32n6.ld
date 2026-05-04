MEMORY {
         BOOT  (rx)  : ORIGIN = 0x34180400, LENGTH = 510K
         VTBL  (rx)  : ORIGIN = 0x24000000, LENGTH = 1K
         RAM   (xrw) : ORIGIN = 0x24000400, LENGTH = 256K
         ITCM  (rx)  : ORIGIN = 0x00001000, LENGTH = 60K
}

NOCROSSREFS(.boot .text)

SECTIONS {
        .boot : {
                . = ALIGN(32);
                KEEP(*(.bootloader_vector))
                *(bltext)
                *(bldata)
                . = ALIGN(128);
                *(blvtbl)
                . = ALIGN(32);
        } >BOOT

        _boot_start = ADDR(.boot);
        _boot_end = _boot_start + SIZEOF(.boot);

        .isr_vector : {
                . = ALIGN(4);
                KEEP(*(.isr_vector))
                . = ALIGN(4);
        } >VTBL

        .fastcode : {
                . = ALIGN(8);
                _sfastcode = .;
                *(fastcode)
                *(fastcode.*)
                . = ALIGN(8);
                _efastcode = .;
        } >ITCM

        .text : {
                . = ALIGN(8);
                *(.text)
                *(.text.*)
                _text_end = .;
                *(.rodata)
                *(.rodata.*)
                . = ALIGN(4);
                _ota_flasher_begin = .;
                KEEP(*(ota_flasher))
                _ota_flasher_end = .;
        } >RAM

        .ARM.extab : {
                . = ALIGN(4);
                *(.ARM.extab*)
                *(.gnu.linkonce.armextab.*)
        } >RAM

        .ARM.exidx : {
                . = ALIGN(4);
                 __exidx_start = .;
                 *(.ARM.exidx*)
                 *(.gnu.linkonce.armexidx.*)
                 __exidx_end = .;
        } >RAM

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

                INCLUDE cpu/section_arrays.ld

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
                 _etext = .;
        } >RAM

        .data : {
                . = ALIGN(8);
                _sdata = .;
                *(ramcode)
                *(.data)
                *(.data.*)
                . = ALIGN(4);
                _edata = .;
        } >RAM

        .build_id : {
                . = ALIGN(4);
                PROVIDE(_build_id = .);
                KEEP(*(.note.gnu.build-id))
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
