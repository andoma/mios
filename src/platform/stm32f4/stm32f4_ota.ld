MEMORY {
         BOOT  (rx) : ORIGIN = 0x08000000, LENGTH = 16K
         VTBL  (rx) : ORIGIN = 0x08004000, LENGTH = 448
         FLASH (rx) : ORIGIN = 0x080041c0, LENGTH = 512K
         RAM   (xrw): ORIGIN = 0x20000300, LENGTH = 192K
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
