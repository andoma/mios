MEMORY {
           INIT (rx): ORIGIN = 0x272000000, LENGTH = 4M
           TEXT (rx): ORIGIN = 0xffff000000001000, LENGTH = 1020K
           BSS  (rw): ORIGIN = 0xffff000000200000, LENGTH = 2M
}

INCLUDE cpu/aarch64/aarch64.ld
