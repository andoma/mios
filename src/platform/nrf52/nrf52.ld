MEMORY {
         VTBL  (rx) : ORIGIN = 0x00000000, LENGTH = 256
         FLASH (rx) : ORIGIN = 0x00000100, LENGTH = 512K
         RAM   (xrw): ORIGIN = 0x20000200, LENGTH = 192K
}

INCLUDE cpu/cortexm/cortexm.ld
