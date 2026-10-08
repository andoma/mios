MEMORY {
 FLASH (rx) : ORIGIN = 0, LENGTH = 256K
 RAM (rwx) : ORIGIN = 0x20000000, LENGTH = 64K
}
SECTIONS {
 .text : { _stext = .; KEEP(*(.vectors)) *(.text*) *(.rodata*) _text_end = .; } > FLASH
 .bss : { *(.bss*) *(COMMON) } > RAM
}
