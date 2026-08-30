GLOBALDEPS += ${SRC}/shell/shell.mk

SRCS +=	${SRC}/shell/cli.c \
	${SRC}/shell/cli_edit.c \
	${SRC}/shell/monitor.c \
	${SRC}/shell/cmd_gpio.c \
	${SRC}/shell/cmd_i2c.c \
	${SRC}/shell/history.c \

SRCS-${ENABLE_PERFTEST} += \
	${SRC}/shell/perf.c \

SRCS-${ENABLE_VCON} += \
	${SRC}/shell/cmd_vcon.c \

# mcp_uart.c is transport-agnostic (any HDLC-framed stream_t) despite the
# name -- nrf54l.mk also pulls it in directly, unconditionally, for its
# UART use case, independent of this flag.
SRCS-${ENABLE_MCP} += \
	${SRC}/shell/mcp_uart.c \

${MOS}/shell/cli.o : CFLAGS += ${NOFPU}
${MOS}/shell/cli_edit.o : CFLAGS += ${NOFPU}
${MOS}/shell/monitor.o : CFLAGS += ${NOFPU}
${MOS}/shell/cmd_i2c.o : CFLAGS += ${NOFPU}
${MOS}/shell/history.o : CFLAGS += ${NOFPU}
