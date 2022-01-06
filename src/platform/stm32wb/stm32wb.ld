MEMORY {
         VTBL  (rx) : ORIGIN = 0x08000000, LENGTH = 512
         FLASH (rx) : ORIGIN = 0x08000200, LENGTH = 512K
         RAM   (xrw): ORIGIN = 0x20000200, LENGTH = 192K
}

INCLUDE cpu/cortexm/cortexm.ld
