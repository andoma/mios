SRCS +=	${SRC}/shell/cli.c \
	${SRC}/shell/monitor.c \

${MO}/src/shell/%.o : CFLAGS += ${NOFPU}
