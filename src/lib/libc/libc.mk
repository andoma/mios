GLOBALDEPS += ${SRC}/lib/libc/libc.mk

SRCS +=	\
	${SRC}/lib/libc/string.c \
	${SRC}/lib/libc/libc.c \
	${SRC}/lib/libc/stdio.c \
	${SRC}/lib/libc/heap_simple.c \

${MOS}/lib/libc/%.o : CFLAGS += ${NOFPU}

${MOS}/lib/libc/string.o : CFLAGS += ${NOFPU} -ffreestanding -fno-builtin -fno-lto
