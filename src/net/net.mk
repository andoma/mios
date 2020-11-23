SRCS +=	${SRC}/net/pcs/pcs.c

${MO}/src/pcs/pcs.o : CFLAGS += ${NOFPU}
