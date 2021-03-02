MEMORY {
         FLASH (rx) : ORIGIN = 0x08000000, LENGTH = 512K
         RAM   (xrw): ORIGIN = 0x20000200, LENGTH = 192K
}

INCLUDE cpu/cortexm/cortexm.ld
