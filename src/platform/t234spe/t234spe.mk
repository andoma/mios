
ENABLE_TASK_DEBUG := no
ENABLE_MATH := no
ENABLE_UART_16550 := yes

PLATFORM := t234spe

P := ${SRC}/platform/${PLATFORM}
T234 := ${SRC}/platform/t234

GLOBALDEPS += ${P}/${PLATFORM}.mk

CPPFLAGS += -I${P} -I${T234} -include ${PLATFORM}.h

LDSCRIPT = ${P}/${PLATFORM}.ld

include ${SRC}/cpu/aarch32/aarch32.mk

SRCS += ${P}/entry.s \
	${P}/idle.c \
	${P}/t234spe.c \
	${P}/t234spe_hsp.c \
	${P}/t234spe_can.c \
	${P}/tcu.c \
	${P}/systick.c \
	${P}/irq.c \
	${T234}/t234_ast.c

${P}/%.o : CFLAGS += ${NOFPU}
${T234}/%.o : CFLAGS += ${NOFPU}
