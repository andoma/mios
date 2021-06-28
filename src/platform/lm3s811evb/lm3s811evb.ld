MEMORY {
  RAM   (xrw): ORIGIN = 0x20000200, LENGTH = 20K
  VTBL  (rx) : ORIGIN = 0x0,   LENGTH = 1K
  FLASH (rx) : ORIGIN = 0x400, LENGTH = 256K
}

INCLUDE cpu/cortexm/cortexm.ld
