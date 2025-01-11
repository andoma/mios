MEMORY {
         RAM   (xrw) : ORIGIN = 0x0c480000, LENGTH = 128k
}

INCLUDE cpu/aarch32/aarch32.ld
