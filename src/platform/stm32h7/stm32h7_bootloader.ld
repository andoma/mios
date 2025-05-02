MEMORY {
         BOOT  (rx) : ORIGIN = 0x08000000, LENGTH = 128K
         VTBL  (rx) : ORIGIN = 0x08020000, LENGTH = 1K
         FLASH (rx) : ORIGIN = 0x08020400, LENGTH = 383K
         RAM   (xrw): ORIGIN = 0x20000400, LENGTH = 128K
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
