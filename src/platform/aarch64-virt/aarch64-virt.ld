MEMORY {
         ROM   (xr): ORIGIN = 0x00000000, LENGTH = 128M
         RAM   (rw): ORIGIN = 0x40000000, LENGTH = 1024M
}

INCLUDE cpu/aarch64/aarch64.ld
