
P := ${SRC}/platform/lm3s811evb

GLOBALDEPS += ${P}/lm3s811evb.mk
CPPFLAGS += -I${P}

include ${SRC}/cpu/cortexm/cortexm.mk

SRCS += ${P}/platform.c \
	${P}/console.c \
