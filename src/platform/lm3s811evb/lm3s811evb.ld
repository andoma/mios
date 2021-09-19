MEMORY {
  RAM   (xrw): ORIGIN = 0x20000200, LENGTH = 20K
  VTBL  (rx) : ORIGIN = 0x0,   LENGTH = 192
  FLASH (rx) : ORIGIN = 0xc0,  LENGTH = 256K
}

INCLUDE cpu/cortexm/cortexm.ld
