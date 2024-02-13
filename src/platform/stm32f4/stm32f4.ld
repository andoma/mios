MEMORY {
         VTBL  (rx) : ORIGIN = 0x08000000, LENGTH = 448
         FLASH (rx) : ORIGIN = 0x080001c0, LENGTH = 512K
         RAM   (xrw): ORIGIN = 0x20000300, LENGTH = 192K
}

INCLUDE cpu/cortexm/cortexm.ld
