GLOBALDEPS += ${SRC}/lib/libc/libc.mk

SRCS +=	\
	${SRC}/lib/libc/string.c \
	${SRC}/lib/libc/libc.c \
	${SRC}/lib/libc/stdio.c \

# The allocator. heap_simple is the one real targets use; the host builds
# can swap in the mmap-per-allocation debugging allocator in
# src/cpu/host/heap_mmap.c, which provides the same entry points.
ifneq (${ENABLE_HEAP_MMAP},yes)
SRCS += ${SRC}/lib/libc/heap_simple.c
endif

${MOS}/lib/libc/%.o : CFLAGS += ${NOFPU}

${MOS}/lib/libc/string.o : CFLAGS += ${NOFPU} -ffreestanding -fno-builtin -fno-lto
