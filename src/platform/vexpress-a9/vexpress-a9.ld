MEMORY {
         RAM   (xrw) : ORIGIN = 0x60000000, LENGTH = 32M
}

INCLUDE cpu/aarch32/aarch32.ld
