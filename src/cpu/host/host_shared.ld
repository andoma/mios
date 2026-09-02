/*
 * Linker script for the "host" CPU: static, non-PIE x86-64 Linux ELF.
 *
 * Mirrors cortexm.ld in what it exports (init/fini arrays, section
 * arrays for CLI commands, drivers, etc. and the version blobs) so
 * kernel/mios.c and version.c work unchanged.
 */

ENTRY(_start)

SECTIONS {
        . = SIZEOF_HEADERS;

        .text : {
                . = ALIGN(16);
                _stext = .;
                *(.text)
                *(.text.*)
                *(fastcode)
                *(fastcode.*)
                _text_end = .;
        }

        .rodata : {
                . = ALIGN(16);
                *(.rodata)
                *(.rodata.*)
                . = ALIGN(8);
                _ota_flasher_begin = .;
                KEEP(*(ota_flasher))
                _ota_flasher_end = .;
        }

        .eh_frame : {
                KEEP(*(.eh_frame))
        }

        .build_id : {
                . = ALIGN(4);
                PROVIDE(_build_id = .);
                KEEP(*(.note.gnu.build-id))
        }

        /* Everything below is writable (init arrays and the version blobs
           are non-const objects) and lives in the RW segment. */
        . = ALIGN(0x1000);

        .inits : {
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

                INCLUDE cpu/section_arrays.ld

                /* Host test suites, see platform/host/hosttest.h */
                . = ALIGN(8);
                _hosttest_array_begin = .;
                KEEP(*(SORT_BY_NAME(hosttest.*)))
                _hosttest_array_end = .;
        }

        .miosversion : {
                . = ALIGN(4);
                PROVIDE(_miosversion = .);
                KEEP(*(miosversion))
        }

        .appversion : {
                . = ALIGN(4);
                PROVIDE(_appversion = .);
                KEEP(*(appversion))
        }

        .appname : {
                . = ALIGN(4);
                PROVIDE(_appname = .);
                KEEP(*(appname))
        }

        .mcufamily : {
                . = ALIGN(4);
                PROVIDE(_mcu_family = .);
                KEEP(*(mcufamily))
        }

        .cmdline_info : {
                . = ALIGN(4);
                KEEP(*(cmdline_info))
        }

        .data : {
                . = ALIGN(16);
                _sdata = .;
                *(ramcode)
                *(.data)
                *(.data.*)
                . = ALIGN(8);
                _edata = .;
        }

        .bss : {
                . = ALIGN(16);
                _sbss = .;
                *(.bss)
                *(.bss.*)
                *(COMMON)
                . = ALIGN(16);
                _ebss = .;
        }

        /DISCARD/ : {
                *(.note.GNU-stack)
                *(.comment)
        }
}
