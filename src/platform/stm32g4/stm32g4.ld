MEMORY {
         VTBL  (rx) : ORIGIN = 0x08000000, LENGTH = 1K
         FLASH (rx) : ORIGIN = 0x080001d8, LENGTH = 512K
         RAM   (xrw): ORIGIN = 0x20000300, LENGTH = 192K
}

INCLUDE cpu/cortexm/cortexm.ld
