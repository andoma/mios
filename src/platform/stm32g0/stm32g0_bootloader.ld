MEMORY {
         BOOT  (rx) : ORIGIN = 0x08000000, LENGTH = 2048
         VTBL  (rx) : ORIGIN = 0x08000800, LENGTH = 192
         FLASH (rx) : ORIGIN = 0x080008c0, LENGTH = 64K
         RAM   (xrw): ORIGIN = 0x20000200, LENGTH = 8K
}

NOCROSSREFS(.boot .text)

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
