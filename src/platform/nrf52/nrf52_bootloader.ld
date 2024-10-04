MEMORY {
         BOOT  (rx) : ORIGIN = 0x00000000, LENGTH = 4K
         VTBL  (rx) : ORIGIN = 0x00001000, LENGTH = 256
         FLASH (rx) : ORIGIN = 0x00001100, LENGTH = 512K
         RAM   (xrw): ORIGIN = 0x20000200, LENGTH = 192K
}

NOCROSSREFS(.boot .text)

SECTIONS {

	.boot : {
                . = ALIGN(4);
                KEEP(*(.bootloader_vector))
                *(bltext)
                *(bldata)
                . = ALIGN(8);
		_ebltext = .;
        } >BOOT
}

INCLUDE cpu/cortexm/cortexm.ld
