P := ${SRC}/platform/t234ccplex

GLOBALDEPS += ${P}/t234ccplex.mk

CFLAGS += -mcpu=cortex-a78ae

CPPFLAGS += -iquote${P} -I${SRC}/platform/t234 -include ${P}/t234ccplex.h

LDSCRIPT = ${P}/t234ccplex.ld

include ${SRC}/cpu/aarch64/aarch64.mk

SRCS += ${P}/t234ccplex.c \
	${P}/t234ccplex_hsp.c \
	${P}/t234ccplex_ari.c \
	${P}/t234ccplex_bpmp.c \
	${P}/t234ccplex_qspi.c \
	${P}/efiruntime.c \
	${P}/asm.s \

ENABLE_TASK_ACCOUNTING := no

${MOS}/src/platform/t234ccplex/efiruntime.o : CFLAGS += -fpic
