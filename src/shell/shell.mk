SRCS +=	${SRC}/shell/cli.c \
	${SRC}/shell/monitor.c \
	${SRC}/shell/msmp.c \
	${SRC}/shell/cmd_i2c.c \

${MOS}/shell/%.o : CFLAGS += ${NOFPU}
