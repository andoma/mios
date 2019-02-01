CPU := cortexm

C := src/cpu/${CPU}
include ${C}/cpu.mk

SRCS += ${P}/platform.c \
	${P}/console.c \

