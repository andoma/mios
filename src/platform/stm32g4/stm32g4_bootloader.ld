MEMORY {
         BOOT  (rx) : ORIGIN = 0x08000000, LENGTH = 2048
         VTBL  (rx) : ORIGIN = 0x08000800, LENGTH = 1K
         FLASH (rx) : ORIGIN = 0x080009d8, LENGTH = 192K
         RAM   (xrw): ORIGIN = 0x20000200, LENGTH = 8K
}

SECTIONS {
        .boot : {
                . = ALIGN(4);
                KEEP(*(.bootloader_vector))
                *(bltext)
                *(bldata)
                . = ALIGN(4);
        } >BOOT

}

INCLUDE cpu/cortexm/cortexm.ld
