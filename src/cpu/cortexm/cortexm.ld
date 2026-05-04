/* Included by platform specific linker script */

SECTIONS {
        .isr_vector : {
                . = ALIGN(4);
                KEEP(*(.isr_vector))
                . = ALIGN(4);
        } >VTBL

        .text : {
                . = ALIGN(8);
                _stext = .;
                *(.text)
                *(.text.*)
                *(fastcode)
                *(fastcode.*)
                _text_end = .;
                *(.rodata)
                *(.rodata.*)
                . = ALIGN(4);
                _ota_flasher_begin = .;
                KEEP(*(ota_flasher))
                _ota_flasher_end = .;
        } >FLASH

        .ARM.extab : {
                . = ALIGN(4);
                *(.ARM.extab*)
                *(.gnu.linkonce.armextab.*)
        } > FLASH

        .ARM.exidx : {
                . = ALIGN(4);
                 __exidx_start = .;
                 *(.ARM.exidx*)
                 *(.gnu.linkonce.armexidx.*)
                  __exidx_end = .;
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

                INCLUDE cpu/section_arrays.ld

        } >FLASH

        .miosversion : {
                . = ALIGN(4);
                PROVIDE(_miosversion = .);
                KEEP(*(miosversion))
        } >FLASH

        .appversion : {
                . = ALIGN(4);
                PROVIDE(_appversion = .);
                KEEP(*(appversion))
        } >FLASH

        .appname : {
                . = ALIGN(4);
                PROVIDE(_appname = .);
                KEEP(*(appname))
                . = ALIGN(8);
                 _etext = .;
        } >FLASH

        .data : {
                . = ALIGN(8);
                _sdata = .;
                *(ramcode)
                *(.data)
                *(.data.*)
                . = ALIGN(4);
                _edata = .;
        } >RAM AT >FLASH

        .build_id : {
                . = ALIGN(4);
                PROVIDE(_build_id = .);
                KEEP(*(.note.gnu.build-id))
        } >FLASH

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
