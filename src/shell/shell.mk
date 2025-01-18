GLOBALDEPS += ${SRC}/shell/shell.mk

SRCS +=	${SRC}/shell/cli.c \
	${SRC}/shell/monitor.c \
	${SRC}/shell/cmd_i2c.c \
	${SRC}/shell/history.c \
	${SRC}/shell/perf.c \

${MOS}/shell/cli.o : CFLAGS += ${NOFPU}
${MOS}/shell/monitor.o : CFLAGS += ${NOFPU}
${MOS}/shell/cmd_i2c.o : CFLAGS += ${NOFPU}
${MOS}/shell/history.o : CFLAGS += ${NOFPU}
