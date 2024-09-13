SRCS +=	${SRC}/shell/cli.c \
	${SRC}/shell/monitor.c \
	${SRC}/shell/cmd_i2c.c \
	${SRC}/shell/history.c \

${MOS}/shell/%.o : CFLAGS += ${NOFPU}
