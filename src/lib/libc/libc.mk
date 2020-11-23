GLOBALDEPS += ${SRC}/lib/libc/libc.mk

SRCS +=	${SRC}/lib/libc/libc.c \
	${SRC}/lib/libc/stdio.c \
	${SRC}/lib/libc/heap_simple.c \

${MO}/src/lib/libc/%.o : CFLAGS += ${NOFPU}
