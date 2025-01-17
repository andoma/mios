MEMORY {
         VTBL  (rx) : ORIGIN = 0x08000000, LENGTH = 1K
         FLASH (rx) : ORIGIN = 0x08000400, LENGTH = 512K
         RAM   (xrw): ORIGIN = 0x20000400, LENGTH = 128K
}

INCLUDE cpu/cortexm/cortexm.ld
