/* details on syntax can be found in GCC linker manual */

MEMORY {
  RAM   (xrw): ORIGIN = 0x20000200, LENGTH = 20K
  FLASH (rx) : ORIGIN = 0x0, LENGTH = 256K
}

SECTIONS {
        .isr_vector : {
                . = ALIGN(4);
                KEEP(*(.isr_vector))    /* vector table */
                . = ALIGN(4);
        } >FLASH

        .text : {
                . = ALIGN(4);
                *(.text)                /* Program code */
                *(.text.*)
                *(.rodata)              /* read only data */
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
                _etext = .;             /* _etext is end of program code */
        } >FLASH                        /* this section is code, hence gos to FLASH */

        .data : AT ( _etext ) {         /* AT specifies the LMA (load memory address) */ 
                . = ALIGN(4);           /* also, this section is for initialized
                                             global/static variables */
                _sdata = .;
                *(.data)
                *(.data.*)
                . = ALIGN(4);
                _edata = .;
        } >RAM                          /* this section go into this RAM */

        .bss : {                        /* this section is for uninitialized global/static
                                        variables, which we set to zero */
                . = ALIGN(4);
                _sbss = .;              /* the . sets the symbol to value of current counter */
                *(.bss)
                *(COMMON)
                . = ALIGN(16);
                _ebss = .;
        } >RAM
}
