MEMORY {
         VTBL  (rx) : ORIGIN = 0x08000000, LENGTH = 192
         FLASH (rx) : ORIGIN = 0x080000c0, LENGTH = 64K
         RAM   (xrw): ORIGIN = 0x20000200, LENGTH = 8K
}

INCLUDE cpu/cortexm/cortexm.ld
