
ENABLE_TASK_DEBUG := no
ENABLE_MATH := no
ENABLE_UART_16550 := yes

PLATFORM := tegra234-spe

P := ${SRC}/platform/${PLATFORM}
T234 := ${SRC}/platform/tegra234

GLOBALDEPS += ${P}/${PLATFORM}.mk

CPPFLAGS += -I${P} -I${SRC}/platform/tegra234 -include ${PLATFORM}.h

LDSCRIPT = ${P}/${PLATFORM}.ld

include ${SRC}/cpu/aarch32/aarch32.mk

SRCS += ${P}/entry.s \
	${P}/tegra234-spe.c \
	${P}/tegra234-spe_hsp.c \
	${P}/tcu.c \
	${P}/ivc.c \
	${P}/systick.c \
	${P}/irq.c \
	${T234}/tegra234_ast.c

${P}/%.o : CFLAGS += ${NOFPU}
${T234}/%.o : CFLAGS += ${NOFPU}
