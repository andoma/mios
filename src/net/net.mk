SRCS +=	${SRC}/net/pcs/pcs.c ${SRC}/net/pcs_shell.c

${MO}/src/pcs/pcs.o : CFLAGS += ${NOFPU}
